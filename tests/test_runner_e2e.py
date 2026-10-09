"""Windows E2E: real fixture launch -> runtime attach -> input -> observation -> reports.
Only the allowlisted test fixture receives window messages; never SendInput.
"""
import argparse,json,pathlib,queue,subprocess,tempfile,threading,time

class Mcp:
    def __init__(self,exe,args,cwd):
        self.p=subprocess.Popen([str(exe),'mcp',*args],cwd=cwd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
        self.q=queue.Queue();self.err=[];self.seq=0
        def reader():
            for line in self.p.stdout:self.q.put(line)
            self.q.put(None)
        threading.Thread(target=reader,daemon=True).start()
        threading.Thread(target=lambda:self.err.extend(self.p.stderr.readlines()),daemon=True).start()
        self.request('initialize',{'protocolVersion':'2025-11-25'})
    def request(self,method,params):
        self.seq+=1; i=self.seq
        self.p.stdin.write(json.dumps({'jsonrpc':'2.0','id':i,'method':method,'params':params})+'\n');self.p.stdin.flush()
        end=time.monotonic()+30
        while True:
            raw=self.q.get(timeout=max(.01,end-time.monotonic()))
            assert raw is not None, 'MCP exited: '+''.join(self.err)
            try:r=json.loads(raw)
            except Exception:raise AssertionError('Non-JSON on MCP stdout: '+repr(raw))
            if 'id' not in r:continue
            assert r['id']==i,r
            assert 'result' in r,r
            return r['result']
    def call(self,name,**args):return self.request('tools/call',{'name':name,'arguments':args})
    def close(self):
        self.p.stdin.close()
        try:self.p.wait(5)
        except subprocess.TimeoutExpired:self.p.kill();self.p.wait()
    def data(self,name,**args):
        r=self.call(name,**args); assert not r.get('isError'),r
        return r['structuredContent']

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--build-root',required=True);ap.add_argument('--arch',choices=['x64','x86'],required=True);a=ap.parse_args()
    root=pathlib.Path(a.build_root).resolve();exe=root/'cortex_host.exe';target=root/f'cortex_test_target_{a.arch}.exe'
    evidence=root/'test-runner-evidence';evidence.mkdir(exist_ok=True)
    checks=[];pid=0;client=None
    with tempfile.TemporaryDirectory(prefix='cortex-e2e-') as temp:
        tmp=pathlib.Path(temp);manifest=tmp/'target.json';config=tmp/'launch.json'
        config.write_text(json.dumps({'profiles':[{'name':'fixture','executable':str(target),'arguments':['--e2e-manifest',str(manifest)],'allow_stop':True,'allow_reset':True,'allow_attach':True,'allow_input':True,'allow_mouse':True,'test_keys':[32],'max_runs':2,'max_tests':9}]}))
        try:
            client=Mcp(exe,['--launch-config',str(config),'--test-results',str(evidence)],root)
            guide=client.data('cortex_agent_guide')['guide'];assert 'not a universal' in guide;checks.append('built_in_guide')
            started=client.data('cortex_launch',profile='fixture',attach=True,mutation_permission=True)
            pid=started['pid'];assert started['attached'];generation=started['generation'];checks.append('launch_auto_attach')
            assert manifest.exists();fixture=json.loads(manifest.read_text())
            reads=[{'address':fixture['test_value'],'type':'u32'}]
            base={'_cortex_target':pid,'_cortex_generation':generation,'mode':'game','mutation_permission':True,'reads':reads,'steps':[{'vk':32,'tap_ms':40}],'expect':[{'read':0,'op':'increased'}]}
            assert client.call('cortex_test_run',**dict(base,mutation_permission=False))['isError'];checks.append('permission_denied')
            assert client.call('cortex_test_run',**dict(base,steps=[{'vk':65,'tap_ms':40}]))['isError'];checks.append('key_not_allowlisted')
            assert client.call('cortex_test_run',**dict(base,_cortex_generation=generation+1))['isError'];checks.append('stale_generation')
            def run(**changes):
                begun=client.data('cortex_test_run',**dict(base,**changes));id=begun['id']
                return wait(id)
            def wait(id):
                for n in range(500):
                    r=client.data('cortex_test_get',id=id)['run']
                    if r['status'] not in ['queued','running']:return r
                    time.sleep(.02)
                raise AssertionError('Trial did not terminate')
            passed=run();assert passed['outcome']=='passed',passed
            assert passed['before'][0]['value']==0 and passed['after'][0]['value']==1,passed
            saved=passed['id'];checks.append('real_input_before_after')
            control=run(steps=[{'delay_ms':30}],expect=[{'read':0,'op':'unchanged'}]);assert control['outcome']=='passed',control;checks.append('control_trial')
            false=run(expect=[{'read':0,'op':'unchanged'}]);assert false['outcome']=='failed',false;checks.append('false_prediction_rejected')
            missing=run(reads=[{'address':'0x1','type':'u32'}]);assert missing['outcome']=='inconclusive' and missing['error']=='baseline_unreadable',missing;checks.append('unreadable_prevents_input')
            clicked=run(reads=[{'address':fixture['test_clicks'],'type':'u32'}],
                        steps=[{'mouse_click':{'button':'left','x':20,'y':20,'hold_ms':40}}],
                        expect=[{'read':0,'op':'increased'}])
            assert clicked['outcome']=='passed' and clicked['before'][0]['value']==0 and clicked['after'][0]['value']==1,clicked
            checks.append('window_bound_mouse_click')
            waited=run(reads=[{'address':fixture['frame'],'type':'u32'}],
                       steps=[{'wait_for':{'read':0,'op':'increased'},'timeout_ms':1000}],
                       expect=[{'read':0,'op':'increased'}])
            assert waited['outcome']=='passed' and waited['steps'][0]['matched'],waited
            checks.append('condition_wait_real_process')
            slow=client.data('cortex_test_run',**dict(base,steps=[{'vk':32,'tap_ms':500},{'delay_ms':1000}]))
            time.sleep(.05)
            blocked=client.call('cortex_stop',profile='fixture',pid=pid,mutation_permission=True);assert blocked['isError'],blocked
            blocked_reset=client.call('cortex_restart',profile='fixture',pid=pid,generation=generation,attach=True,mutation_permission=True)
            assert blocked_reset['isError'] and blocked_reset['structuredContent']['error']['code']=='test_in_progress',blocked_reset
            checks.append('no_restart_during_active_test')
            client.data('cortex_test_cancel',id=slow['id']);cancelled=wait(slow['id']);assert cancelled['status']=='cancelled',cancelled
            assert cancelled['cleanup']['released'];checks.append('cancel_and_release')
            used=client.data('cortex_launch_status',profile='fixture')['profile']
            assert used['tests']==7 and used['max_tests']==9,used
            checks.append('operator_test_count')
            directory=pathlib.Path(passed['report_directory']);assert (directory/'result.json').exists() and (directory/'investigation.md').exists();checks.append('json_markdown_reports')
            stale=client.call('cortex_restart',profile='fixture',pid=pid,generation=generation+1,attach=True,mutation_permission=True)
            assert stale['isError'] and stale['structuredContent']['error']['code']=='restart_process_identity_mismatch',stale
            checks.append('restart_stale_identity_denied')
            restarted=client.data('cortex_restart',profile='fixture',pid=pid,generation=generation,attach=True,mutation_permission=True)
            pid=restarted['pid']
            assert restarted['generation']!=generation and restarted['attached'] and restarted['reset_scope']=='process_only',restarted
            assert restarted['previous_generation']==generation
            checks.append('atomic_profile_restart_and_attach')
            fixture=json.loads(manifest.read_text())
            base.update({'_cortex_target':pid,'_cortex_generation':restarted['generation'],
                         'reads':[{'address':fixture['test_value'],'type':'u32'}]})
            fresh_control=run(steps=[{'delay_ms':50}],expect=[{'read':0,'op':'unchanged'}])
            fresh_action=run()
            assert fresh_control['outcome']=='passed' and fresh_action['outcome']=='passed',fresh_action
            assert fresh_control['before'][0]['value']==fresh_action['before'][0]['value']==0,(fresh_control,fresh_action)
            comparisons=client.data('cortex_test_compare',first=fresh_control['id'],second=fresh_action['id'])
            assert comparisons['comparison']=='aligned' and comparisons['reads'][0]['candidate_difference'],comparisons
            checks.append('controlled_pair_after_process_restart')
            denied_report=client.call('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Damage | report',
                mutation_permission=False)
            assert denied_report['isError'],denied_report
            checks.append('report_permission_denied')
            combined=client.data('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Damage | report',
                mutation_permission=True)
            report_id=combined['id']
            folder=pathlib.Path(combined['report_directory'])
            assert (folder/'investigation.md').exists() and (folder/'report.json').exists()
            assert combined['report']['comparison']['comparison']=='aligned'
            checks.append('multi_trial_report_persisted')
            limit=client.call('cortex_test_run',**base)
            assert limit['isError'] and limit['structuredContent']['error']['code']=='test_run_limit_reached',limit
            checks.append('operator_test_budget')
            exhausted_restart=client.call('cortex_restart',profile='fixture',pid=pid,
                                           generation=restarted['generation'],attach=True,mutation_permission=True)
            assert exhausted_restart['isError'] and exhausted_restart['structuredContent']['error']['code']=='launch_profile_run_limit_reached',exhausted_restart
            checks.append('restart_respects_run_budget')
            client.data('cortex_stop',profile='fixture',pid=pid,mutation_permission=True);pid=0
            client.close();client=None
            client=Mcp(exe,['--test-results',str(evidence)],root)
            archived=client.data('cortex_test_get',id=saved);assert archived['archived'] and archived['run']['outcome']=='passed';checks.append('report_reloaded_new_agent')
            combined_reloaded=client.data('cortex_test_report_get',id=report_id)
            assert combined_reloaded['report']['id']==report_id and combined_reloaded['untrusted_data']
            checks.append('final_report_reloaded_new_agent')
            assert len(client.data('cortex_test_list')['runs'])>=5
        finally:
            if pid:subprocess.run(['taskkill','/PID',str(pid),'/F'],capture_output=True)
            if client:client.close()
    (evidence/'e2e-summary.json').write_text(json.dumps({'architecture':a.arch,'passed':checks,'count':len(checks)},indent=2))
    print('PASS Windows '+a.arch+': '+str(len(checks))+' checks: '+', '.join(checks))
if __name__=='__main__':main()
