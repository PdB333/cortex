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
        config.write_text(json.dumps({'profiles':[{'name':'fixture','executable':str(target),'arguments':['--e2e-manifest',str(manifest)],'allow_stop':True,'allow_reset':True,'allow_attach':True,'allow_input':True,'allow_mouse':True,'test_keys':[32],'max_runs':2,'max_tests':13}]}))
        try:
            # A launch profile may attach the runtime, but arming a breakpoint changes the
            # target: with --allow-state alone it must be refused (write_authority_required).
            gate=Mcp(exe,['--tools','all','--allow-state','--launch-config',str(config),'--test-results',str(evidence)],root)
            try:
                gate_start=gate.data('cortex_launch',profile='fixture',attach=True,mutation_permission=True)
                assert gate_start['attached'];gate_pid=gate_start['pid']
                for _ in range(100):
                    if manifest.exists():break
                    time.sleep(.05)
                refused=gate.call('debug_breakpoint_add',address=json.loads(manifest.read_text())['test_value'],
                                  kind='hw_write',size=4,action='log',mutation_permission=True)
                assert refused['isError'] and refused['structuredContent']['result']['error']=='write_authority_required',refused
                gate.data('cortex_stop',profile='fixture',pid=gate_pid,mutation_permission=True)
                manifest.unlink(missing_ok=True)
                checks.append('launch_attach_without_allow_writes_cannot_arm_breakpoint')
            finally:
                gate.close()
            # This client may also arm breakpoints (it records the hardware-write log below),
            # so the person grants --allow-writes, which includes --allow-state.
            client=Mcp(exe,['--tools','all','--allow-writes','--launch-config',str(config),'--test-results',str(evidence)],root)
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
            # A previously-authorized debugger log breakpoint can be observed
            # during an ordinary game input trial without the runner arming it.
            breakpoint=client.data('debug_breakpoint_add',
                address=fixture['test_value'],kind='hw_write',size=4,
                action='log',mutation_permission=True)
            assert breakpoint['result']['ok'],breakpoint
            breakpoint_id=int(breakpoint['result']['id'])
            bad_bp=client.call('cortex_test_run',**dict(base,breakpoints=[breakpoint_id+1000]))
            assert bad_bp['isError'] and bad_bp['structuredContent']['error']['code']=='breakpoint_not_logging',bad_bp
            checks.append('unknown_breakpoint_denied')
            passed=run(breakpoints=[breakpoint_id]);assert passed['outcome']=='passed',passed
            code=passed['code_evidence']
            assert code['status']=='observed' and code['breakpoints'][0]['new_hits']>=1,code
            assert code['breakpoints'][0]['events'] and                 code['breakpoints'][0]['truncated_or_missing'] is False,code
            checks.append('runtime_instruction_hit_recorded')
            code_context=client.data('cortex_test_code_context',
                test_id=passed['id'],_cortex_target=pid,_cortex_generation=generation)
            assert code_context['ok'] and code_context['locations'],code_context
            first_ip=code_context['locations'][0]
            assert first_ip['address_role']=='debugger_instruction_pointer' and                 first_ip['module'] is not None and                 first_ip['module']['name'].lower()==target.name.lower() and                 first_ip['module']['rva'].startswith('0x'),code_context
            assert first_ip['symbol']['status'] in ('resolved','unavailable'),first_ip
            assert first_ip['disassembly']['status']=='readable' and                 1<=len(first_ip['disassembly']['instructions'])<=3 and                 first_ip['disassembly']['source']=='host_external_memory',first_ip
            if a.arch=='x64':
                unwind=first_ip['unwind_range']
                assert unwind['status']=='unwind_range' and                     unwind['source']=='live_pe_exception_directory' and                     int(unwind['begin_rva'],16)<=int(first_ip['module']['rva'],16) and                     int(first_ip['module']['rva'],16)<int(unwind['end_rva'],16),unwind
                checks.append('x64_live_pe_unwind_range')
            else:
                assert first_ip['unwind_range']['status']=='unavailable',first_ip
                checks.append('x86_no_unwind_range_invented')
            checks.append('module_rva_and_disassembly_from_recorded_instruction')
            disabled=client.data('cortex_test_code_context',
                test_id=passed['id'],_cortex_target=pid,_cortex_generation=generation,
                symbols=False,disassemble=False,unwind=False,max_locations=1)
            assert disabled['locations'][0]['disassembly']['status']=='not_requested' and                 disabled['locations'][0]['unwind_range']['status']=='not_requested' and                 disabled['locations'][0]['symbol']['status']=='unavailable',disabled
            checks.append('bounded_read_only_code_lookup')
            bad_ctx=client.call('cortex_test_code_context',test_id=passed['id'],
                _cortex_target=pid,_cortex_generation=generation,max_locations=0)
            assert bad_ctx['isError'] and                 bad_ctx['structuredContent']['error']['code']=='invalid_code_context_request',bad_ctx
            checks.append('invalid_code_context_budget_rejected')
            removed=client.data('debug_breakpoint_delete',id=breakpoint_id,
                                mutation_permission=True)
            assert removed['result']['ok'],removed
            assert passed['before'][0]['value']==0 and passed['after'][0]['value']==1,passed
            saved=passed['id'];checks.append('real_input_before_after')
            # Knowledge link checks the actual completed run and the current
            # process identity before adding a provenance reference. It must
            # not upgrade a hypothesis into a semantic "verified" fact.
            knowledge_id='fixture.HealthCandidate_'+__import__('uuid').uuid4().hex
            created=client.data('project_knowledge_put',id=knowledge_id,kind='field',
                statement='Candidate field involved in fixture input handling',
                status='hypothesis',mutation_permission=True)
            record=created['result']['record']
            assert record['revision']==1 and record['status']=='hypothesis',created
            link_args={'test_id':saved,'knowledge_id':knowledge_id,'expected_revision':1,
                       '_cortex_target':pid,'_cortex_generation':generation,
                       'mutation_permission':True}
            denied_link=client.call('cortex_test_link',**dict(link_args,mutation_permission=False))
            assert denied_link['isError'],denied_link
            checks.append('knowledge_link_permission_denied')
            linked=client.data('cortex_test_link',**link_args)
            assert linked['revision']==2 and linked['claim_status']=='hypothesis' and \
                linked['semantic_verified'] is False and linked['evidence_count']==1,linked
            checks.append('trial_linked_to_knowledge')
            reloaded_claim=client.data('project_knowledge_list')['result']['records'][knowledge_id]
            assert reloaded_claim['evidence'][0]['source']=='cortex_test' and \
                reloaded_claim['evidence'][0]['reference']==saved,reloaded_claim
            assert reloaded_claim['status']=='hypothesis' and \
                reloaded_claim['last_verification']['status']=='not_run',reloaded_claim
            checks.append('knowledge_evidence_persisted')
            rival_id='fixture.AlternateCandidate_'+__import__('uuid').uuid4().hex
            rival=client.data('project_knowledge_put',id=rival_id,kind='behavior',
                statement='Alternative prediction: input leaves the counter unchanged',
                status='hypothesis',mutation_permission=True)
            assert rival['result']['record']['status']=='hypothesis',rival
            stale_link=client.call('cortex_test_link',**link_args)
            assert stale_link['isError'] and \
                stale_link['structuredContent']['error']['code']=='knowledge_revision_conflict',stale_link
            duplicate_link=client.call('cortex_test_link',**dict(link_args,expected_revision=2))
            assert duplicate_link['isError'] and \
                duplicate_link['structuredContent']['error']['code']=='test_evidence_already_linked',duplicate_link
            checks.append('knowledge_revision_and_duplicate_guards')
            # Preserve only genuinely observed debugger IPs, mapped to a
            # versioned module+RVA. A client cannot submit a guessed code IP.
            code_link_args=dict(link_args,expected_revision=2,max_locations=1)
            denied_code=client.call('cortex_test_code_link',
                                    **dict(code_link_args,mutation_permission=False))
            assert denied_code['isError'],denied_code
            checks.append('code_link_permission_denied')
            linked_code=client.data('cortex_test_code_link',**code_link_args)
            assert linked_code['revision']==3 and linked_code['added']==1 and \
                linked_code['claim_status']=='hypothesis' and \
                linked_code['semantic_verified'] is False,linked_code
            versioned=client.data('project_knowledge_list')['result']['records'][knowledge_id]
            ref=versioned['code_references'][0]
            assert ref['test_id']==saved and ref['source']=='debugger_observation' and \
                ref['module'].lower()==target.name.lower() and \
                ref['rva']==first_ip['module']['rva'] and \
                len(ref['file_sha256'])==64 and ref['file_size']>0 and \
                'address' not in ref and 'base' not in ref,versioned
            checks.append('code_reference_persisted_with_binary_hash')
            current_code=client.data('cortex_knowledge_code_check',
                knowledge_id=knowledge_id,_cortex_target=pid,_cortex_generation=generation)
            assert current_code['checked']==1 and current_code['matched']==1 and \
                current_code['results'][0]['status']=='matched' and \
                current_code['results'][0]['address']==first_ip['address'] and \
                current_code['semantic_verified'] is False,current_code
            checks.append('code_reference_revalidated_in_same_generation')
            duplicate_code=client.call('cortex_test_code_link',
                **dict(code_link_args,expected_revision=3))
            assert duplicate_code['isError'] and \
                duplicate_code['structuredContent']['error']['code']=='code_reference_already_linked',duplicate_code
            checks.append('duplicate_code_link_denied')
            control=run(steps=[{'delay_ms':30}],expect=[{'read':0,'op':'unchanged'}]);assert control['outcome']=='passed',control;checks.append('control_trial')
            missing_code=client.call('cortex_test_code_context',
                test_id=control['id'],_cortex_target=pid,
                _cortex_generation=generation)
            assert missing_code['isError'] and                 missing_code['structuredContent']['error']['code']=='no_completed_code_observations',missing_code
            checks.append('no_code_events_cannot_invent_function')
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
            assert used['tests']==7 and used['max_tests']==13,used
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
            carried=client.data('project_knowledge_list')['result']['records'][knowledge_id]
            assert carried['revision']==3 and carried['evidence'][0]['reference']==saved and \
                carried['code_references'][0]['file_sha256']==ref['file_sha256'],carried
            checks.append('knowledge_survives_target_restart')
            # The old absolute IP is invalid; an identical new on-disk file
            # allows Cortex to recompute the offset in the CURRENT module.
            after_restart=client.data('cortex_knowledge_code_check',
                knowledge_id=knowledge_id,_cortex_target=pid,
                _cortex_generation=restarted['generation'])
            assert after_restart['matched']==1 and \
                after_restart['results'][0]['status']=='matched' and \
                after_restart['results'][0]['address'].startswith('0x') and \
                after_restart['semantic_verified'] is False,after_restart
            checks.append('code_reference_revalidated_after_restart')
            old_version=client.call('cortex_knowledge_code_check',
                knowledge_id=knowledge_id,_cortex_target=pid,
                _cortex_generation=generation)
            assert old_version['isError'],old_version
            checks.append('stale_code_check_generation_denied')
            old_generation_link=client.call('cortex_test_link',**dict(link_args,
                expected_revision=3,_cortex_target=pid,
                _cortex_generation=restarted['generation']))
            assert old_generation_link['isError'] and \
                old_generation_link['structuredContent']['error']['code']=='test_target_identity_mismatch',old_generation_link
            checks.append('cross_generation_evidence_revalidated')
            stale_ctx=client.call('cortex_test_code_context',
                test_id=saved,_cortex_target=pid,
                _cortex_generation=restarted['generation'],max_locations=2)
            assert stale_ctx['isError'] and                 stale_ctx['structuredContent']['error']['code']=='test_target_identity_mismatch',stale_ctx
            checks.append('stale_code_context_denied_after_restart')
            fixture=json.loads(manifest.read_text())
            base.update({'_cortex_target':pid,'_cortex_generation':restarted['generation'],
                         'reads':[{'address':fixture['test_value'],'type':'u32'}]})
            fresh_bp=client.data('debug_breakpoint_add',
                address=fixture['test_value'],kind='hw_write',size=4,
                action='log',mutation_permission=True)
            assert fresh_bp['result']['ok'],fresh_bp
            fresh_bp_id=int(fresh_bp['result']['id'])
            fresh_control=run(steps=[{'delay_ms':50}],expect=[{'read':0,'op':'unchanged'}],
                              breakpoints=[fresh_bp_id])
            fresh_action=run(breakpoints=[fresh_bp_id])
            removed=client.data('debug_breakpoint_delete',id=fresh_bp_id,
                                mutation_permission=True)
            assert removed['result']['ok'],removed
            assert fresh_control['outcome']=='passed' and fresh_action['outcome']=='passed',fresh_action
            assert fresh_control['before'][0]['value']==fresh_action['before'][0]['value']==0,(fresh_control,fresh_action)
            comparisons=client.data('cortex_test_compare',first=fresh_control['id'],second=fresh_action['id'])
            assert comparisons['comparison']=='aligned' and comparisons['reads'][0]['candidate_difference'],comparisons
            checks.append('controlled_pair_after_process_restart')
            graph=client.data('cortex_test_evidence_graph',
                control=fresh_control['id'],action=fresh_action['id'])
            assert graph['schema']=='cortex.evidence.graph.v1' and graph['pair_comparable']
            assert graph['semantic_verified'] is False
            assert any(n['kind']=='measured_read' and n['id']=='action_read_0' and
                       n['detail']['changed'] for n in graph['nodes']),graph
            assert all(edge['causal_verified'] is False for edge in graph['edges']),graph
            assert any(node['kind']=='observed_instruction' for node in graph['nodes']),graph
            assert any(edge['relation']=='logged_during' for edge in graph['edges']),graph
            checks.append('bounded_evidence_graph_is_not_causal_proof')
            denied_graph=client.call('cortex_test_evidence_graph',
                control='test_missing',action=fresh_action['id'])
            assert denied_graph['isError'],denied_graph
            checks.append('unknown_graph_evidence_rejected')
            code_pair=comparisons['code_comparison']
            assert code_pair['status']=='aligned' and                 code_pair['breakpoints'][0]['control_hits']==0 and                 code_pair['breakpoints'][0]['action_hits']>=1 and                 code_pair['breakpoints'][0]['candidate_more_during_action'],code_pair
            checks.append('instruction_hits_compared_against_control')
            code_candidates=client.data('cortex_test_code_candidates',
                control=fresh_control['id'],action=fresh_action['id'],
                _cortex_target=pid,_cortex_generation=restarted['generation'])
            assert code_candidates['ok'] and code_candidates['candidates'],code_candidates
            assert code_candidates['semantic_verified'] is False and \
                code_candidates['causal_verified'] is False,code_candidates
            assert any(row['module'].lower()==target.name.lower() and
                       row['action_sampled_events']>row['control_sampled_events']
                       for row in code_candidates['candidates']),code_candidates
            assert all(row['classification']=='unverified_code_candidate'
                       and row['call_relationship_verified'] is False
                       for row in code_candidates['candidates'])
            checks.append('live_code_region_candidates_from_real_instruction_hits')
            stale_candidates=client.call('cortex_test_code_candidates',
                control=fresh_control['id'],action=fresh_action['id'],
                _cortex_target=pid,_cortex_generation=generation)
            assert stale_candidates['isError'],stale_candidates
            checks.append('code_region_candidates_reject_stale_process')
            hypotheses=[
                {'id':knowledge_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'increased'}},
                {'id':rival_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'unchanged'}}
            ]
            denied_report=client.call('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Counter | report',
                hypotheses=hypotheses,mutation_permission=False)
            assert denied_report['isError'],denied_report
            checks.append('report_permission_denied')
            empty=client.call('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Empty predictions',
                hypotheses=[],mutation_permission=True)
            assert empty['isError'] and empty['structuredContent']['error']['code']=='invalid_hypotheses',empty
            checks.append('empty_prediction_list_rejected')
            malformed=client.call('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Bad prediction',
                hypotheses=[dict(hypotheses[0],action={'op':'execute'})],
                mutation_permission=True)
            assert malformed['isError'],malformed
            checks.append('malformed_prediction_rejected')
            combined=client.data('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],title='Counter | report',
                hypotheses=hypotheses,mutation_permission=True)
            report_id=combined['id']
            folder=pathlib.Path(combined['report_directory'])
            assert (folder/'investigation.md').exists() and (folder/'report.json').exists()
            assert combined['report']['comparison']['comparison']=='aligned'
            assert combined['report']['trials'][0]['code_breakpoints'] and                 combined['report']['comparison']['code_comparison']['status']=='aligned',combined
            checks.append('consolidated_report_contains_code_hits')
            checks.append('multi_trial_report_persisted')
            generated=client.data('cortex_test_generate',
                report_id=report_id,
                objective='Find why input changes the fixture counter')
            assert generated['status']=='proposed' and generated['executed'] is False
            assert generated['requires_separate_permission'] and \
                generated['semantic_verified'] is False and \
                generated['generated_hypotheses_are_exploratory']
            probe=generated['candidates'][0]
            assert probe['action']['steps'][0]['vk']==32 and \
                probe['action']['steps'][0]['tap_ms']==80,probe
            assert probe['hypotheses'][0]['action']['value']==1 and \
                probe['hypotheses'][1]['action']['value']==2,probe
            checks.append('generated_duration_experiment_from_real_measurements')
            denied_generated=client.call('cortex_test_generate_prepare',
                report_id=report_id,objective='Probe input duration',
                mutation_permission=False)
            assert denied_generated['isError'],denied_generated
            generated_step=client.data('cortex_test_generate_prepare',
                report_id=report_id,objective='Probe input duration',
                mutation_permission=True)['step']
            generated_step_id=generated_step['id']
            assert generated_step['status']=='awaiting_control' and \
                generated_step['expected_target']['generation']==restarted['generation']
            assert generated_step['choice']['action']['steps'][0]['tap_ms']==80
            reopened_generated=client.data('cortex_test_step_get',id=generated_step_id)
            assert reopened_generated['step']['revision']==1 and \
                reopened_generated['step']['executed_by_step'] is False
            checks.append('generated_candidate_persisted_as_supervised_unexecuted_step')
            invalid_goal=client.call('cortex_test_generate',
                report_id=report_id,objective='X'*161)
            assert invalid_goal['isError'],invalid_goal
            checks.append('generation_rejects_unbounded_goal')
            evaluated=combined['report']['hypothesis_tests']
            assert evaluated['pair_comparable'] and evaluated['summary']=={
                'consistent':1,'contradicted':1,'inconclusive':0},evaluated
            assert evaluated['results'][0]['semantic_verified'] is False and \
                evaluated['results'][0]['id']==knowledge_id and \
                evaluated['results'][1]['assessment']=='contradicted_by_measurements',evaluated
            assert 'contradicted_by_measurements' in (folder/'investigation.md').read_text()
            ledger=client.data('project_knowledge_list')['result']['records']
            assert ledger[knowledge_id]['status']=='hypothesis' and \
                ledger[rival_id]['status']=='hypothesis',ledger
            checks.append('competing_predictions_evaluated_without_status_upgrade')
            graph_with_report=client.data('cortex_test_evidence_graph',
                control=fresh_control['id'],action=fresh_action['id'],
                report_id=report_id)
            hypothesis_nodes=[node for node in graph_with_report['nodes']
                              if node['kind']=='measured_hypothesis']
            assert len(hypothesis_nodes)==2,hypothesis_nodes
            assert {node['detail']['assessment'] for node in hypothesis_nodes}=={
                'consistent_with_measurements','contradicted_by_measurements'}
            assert all(node['detail']['semantic_verified'] is False
                       for node in hypothesis_nodes)
            assert sum(edge['relation']=='predicts_observation'
                       for edge in graph_with_report['edges'])==4,graph_with_report
            checks.append('hypothesis_graph_recomputed_from_measured_trial_pair')
            mismatch=client.call('cortex_test_evidence_graph',
                control=fresh_action['id'],action=fresh_control['id'],
                report_id=report_id)
            assert mismatch['isError'],mismatch
            checks.append('graph_report_pair_mismatch_rejected')
            # Two explanations can both fit the *first* comparison while
            # predicting opposite outcomes on a proposed next experiment.
            unresolved=[
                {'id':knowledge_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'increased'}},
                {'id':rival_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'increased'}}
            ]
            unresolved_report=client.data('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],
                title='Choose next bounded experiment',
                hypotheses=unresolved,mutation_permission=True)
            unresolved_id=unresolved_report['id']
            split_predictions=[
                {'id':knowledge_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'increased'}},
                {'id':rival_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'unchanged'}}
            ]
            repeat={'id':'space_again',
                'control':{'steps':[{'delay_ms':50}],
                           'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'action':{'steps':[{'vk':32,'tap_ms':40}],
                          'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'hypotheses':split_predictions}
            click_probe={'id':'click_probe',
                'control':{'steps':[{'delay_ms':30}],
                           'reads':[{'address':fixture['test_clicks'],'type':'u32'}]},
                'action':{'steps':[{'mouse_click':
                           {'button':'left','x':20,'y':20,'hold_ms':40}}],
                          'reads':[{'address':fixture['test_clicks'],'type':'u32'}]},
                'hypotheses':split_predictions}
            chosen=client.data('cortex_test_choose',report_id=unresolved_id,
                               candidates=[repeat,click_probe])
            assert chosen['status']=='proposed' and \
                chosen['choice']['id']=='click_probe' and \
                chosen['choice']['discriminating_pairs']==1 and \
                chosen['candidates'][0]['already_tested'] is True and \
                chosen['executed'] is False and \
                chosen['requires_separate_permission'] is True,chosen
            checks.append('chooses_novel_discriminating_experiment_without_input')
            all_used=client.data('cortex_test_choose',report_id=unresolved_id,
                candidates=[repeat,click_probe],previous_ids=[clicked['id']])
            assert all_used['status']=='no_new_discriminating_experiment',all_used
            checks.append('chooser_rejects_previously_completed_actions')
            stop=client.data('cortex_test_choose',report_id=report_id,
                             candidates=[repeat,click_probe])
            assert stop['status']=='need_competing_hypotheses',stop
            checks.append('chooser_stops_when_no_competing_hypotheses')
            malformed_choice=client.call('cortex_test_choose',report_id=unresolved_id,
                candidates=[dict(click_probe,action={'steps':
                          [{'vk':32,'tap_ms':9999}],
                          'reads':[{'address':fixture['test_clicks'],'type':'u32'}]})])
            assert malformed_choice['isError'],malformed_choice
            checks.append('chooser_enforces_existing_test_budgets')
            # A read-only choice becomes a durable, supervised step: the
            # operator still calls the regular bounded runner for each arm.
            followup={
                'id':'key_then_settle',
                'control':{'steps':[{'delay_ms':30}],
                           'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'action':{'steps':[{'vk':32,'tap_ms':40},{'delay_ms':50}],
                          'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'hypotheses':split_predictions}
            old_input={
                'id':'already_observed',
                'control':{'steps':[{'delay_ms':30}],
                           'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'action':{'steps':[{'vk':32,'tap_ms':40}],
                          'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'hypotheses':split_predictions}
            third_id='fixture.third_mechanism'
            three_initial=unresolved+[{
                'id':third_id,'read':0,
                'control':{'op':'unchanged'},'action':{'op':'increased'}}]
            three_report=client.data('cortex_test_report',
                ids=[fresh_control['id'],fresh_action['id']],
                title='Three surviving explanations',
                hypotheses=three_initial,mutation_permission=True)
            three_predictions=split_predictions+[{
                'id':third_id,'read':0,
                'control':{'op':'unchanged'},'action':{'op':'increased'}}]
            triple_old=dict(old_input,hypotheses=three_predictions)
            triple_follow=dict(followup,hypotheses=three_predictions)
            denied_preparation=client.call('cortex_test_step_prepare',
                report_id=three_report["id"],candidates=[triple_old,triple_follow],
                mutation_permission=False)
            assert denied_preparation['isError'],denied_preparation
            checks.append('step_persistence_permission_required')
            prepared=client.data('cortex_test_step_prepare',
                report_id=three_report["id"],candidates=[triple_old,triple_follow],
                mutation_permission=True)
            step_id=prepared['step']['id']
            assert prepared['step']['choice']['id']=='key_then_settle' and \
                prepared['step']['status']=='awaiting_control' and \
                prepared['step']['executed_by_step'] is False,prepared
            checks.append('persist_supervised_selected_step')
            prebind=client.call('cortex_test_step_record',id=step_id,
                arm='control',test_id=fresh_control['id'],
                expected_revision=1,mutation_permission=True)
            assert prebind['isError'],prebind
            checks.append('cannot_reuse_source_trial_for_followup')
            step_control=run(
                steps=[{'delay_ms':30}],expect=[{'read':0,'op':'unchanged'}])
            denied_bind=client.call('cortex_test_step_record',id=step_id,
                arm='control',test_id=step_control['id'],
                expected_revision=1,mutation_permission=False)
            assert denied_bind['isError'],denied_bind
            control_bind=client.data('cortex_test_step_record',id=step_id,
                arm='control',test_id=step_control['id'],
                expected_revision=1,mutation_permission=True)
            assert control_bind['step']['status']=='awaiting_action' and \
                control_bind['step']['revision']==2,control_bind
            checks.append('supervised_control_bound_with_revision')
            in_progress=client.data('cortex_test_step_get',id=step_id)
            assert in_progress['step']['status']=='awaiting_action' and \
                in_progress['step']['revision']==2 and \
                in_progress['step']['action_id'] is None,in_progress
            checks.append('supervised_partial_step_observable')
            step_action=run(
                steps=[{'vk':32,'tap_ms':40},{'delay_ms':50}],
                expect=[{'read':0,'op':'increased'}])
            completed=client.data('cortex_test_step_record',id=step_id,
                arm='action',test_id=step_action['id'],
                expected_revision=2,mutation_permission=True)
            assert completed['step']['status']=='completed' and \
                completed['step']['revision']==3 and \
                completed['step']['hypothesis_summary']=={
                    'consistent':2,'contradicted':1,'inconclusive':0},completed
            assert completed['step']['semantic_verified'] is False
            checks.append('supervised_pair_archived_without_semantic_upgrade')
            stale_bind=client.call('cortex_test_step_record',id=step_id,
                arm='action',test_id=step_action['id'],
                expected_revision=2,mutation_permission=True)
            assert stale_bind['isError'],stale_bind
            checks.append('step_revision_conflict_prevents_replay')
            # The first supervised pair leaves two explanations plausible.
            # Choose and persist the next experiment from its archived report.
            second_predictions=[
                {'id':knowledge_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'increased'}},
                {'id':third_id,'read':0,
                 'control':{'op':'unchanged'},'action':{'op':'unchanged'}}
            ]
            old_again={'id':'repeat_first',
                'control':followup['control'],
                'action':followup['action'],
                'hypotheses':second_predictions}
            second_proposal={'id':'probe_again',
                'control':followup['control'],
                'action':{'steps':[{'vk':32,'tap_ms':40},{'delay_ms':90}],
                          'reads':[{'address':fixture['test_value'],'type':'u32'}]},
                'hypotheses':second_predictions}
            next_choice=client.data('cortex_test_step_next',
                id=step_id,expected_revision=3,
                candidates=[old_again,second_proposal])
            assert next_choice['status']=='proposed' and \
                next_choice['choice']['id']=='probe_again' and \
                next_choice['candidates'][0]['already_tested'] is True and \
                next_choice['iteration']==2 and \
                len(next_choice['history_ids'])>=4 and \
                next_choice['executed'] is False,next_choice
            checks.append('next_step_reuses_archived_history_and_avoids_repeat')
            not_authorized=client.call('cortex_test_step_continue',
                id=step_id,expected_revision=3,
                candidates=[old_again,second_proposal],
                mutation_permission=False)
            assert not_authorized['isError'],not_authorized
            persisted=client.data('cortex_test_step_continue',
                id=step_id,expected_revision=3,
                candidates=[old_again,second_proposal],
                mutation_permission=True)
            second_id=persisted['step']['id']
            assert persisted['status']=='prepared' and \
                persisted['step']['parent_step_id']==step_id and \
                persisted['step']['root_step_id']==step_id and \
                persisted['step']['iteration']==2 and \
                persisted['step']['status']=='awaiting_control' and \
                persisted['parent_step']['revision']==4 and \
                persisted['executed'] is False,persisted
            checks.append('second_supervised_step_persisted_without_input')
            no_parallel=client.data('cortex_test_step_next',
                id=step_id,expected_revision=4,
                candidates=[old_again,second_proposal])
            assert no_parallel['status']=='continuation_already_prepared',no_parallel
            checks.append('concurrent_second_children_prevented')
            trial2control=run(steps=[{'delay_ms':30}],
                              expect=[{'read':0,'op':'unchanged'}])
            record2control=client.data('cortex_test_step_record',
                id=second_id,arm='control',test_id=trial2control['id'],
                expected_revision=1,mutation_permission=True)
            assert record2control['step']['status']=='awaiting_action',record2control
            trial2action=run(steps=[{'vk':32,'tap_ms':40},{'delay_ms':90}],
                             expect=[{'read':0,'op':'increased'}])
            record2action=client.data('cortex_test_step_record',
                id=second_id,arm='action',test_id=trial2action['id'],
                expected_revision=2,mutation_permission=True)
            assert record2action['step']['status']=='completed' and \
                record2action['step']['hypothesis_summary']=={
                    'consistent':1,'contradicted':1,'inconclusive':0},record2action
            checks.append('two_supervised_rounds_terminate_with_evidence')
            final_stop=client.data('cortex_test_step_next',
                id=second_id,expected_revision=3,
                candidates=[second_proposal])
            assert final_stop['status']=='need_competing_hypotheses',final_stop
            checks.append('bounded_loop_stops_when_only_one_model_remains')
            # Whole investigation report over both genuine supervised trials.
            global_summary=client.data('cortex_test_investigation_summary',id=second_id)
            assert global_summary['round_count']==2 and \
                global_summary['rounds'][0]['summary']['consistent']==2 and \
                global_summary['rounds'][1]['summary']['consistent']==1 and \
                global_summary['rounds'][1]['summary']['contradicted']==1 and \
                global_summary['verified_semantics'] is False,global_summary
            checks.append('whole_investigation_rechecks_two_rounds')
            no_report=client.call('cortex_test_investigation_report',
                id=second_id,mutation_permission=False)
            assert no_report['isError'],no_report
            archive=client.data('cortex_test_investigation_report',
                id=second_id,mutation_permission=True)
            report_path=pathlib.Path(archive['path'])
            assert (report_path/'report.json').is_file() and \
                (report_path/'investigation.md').is_file(),archive
            assert 'Round' in (report_path/'investigation.md').read_text()
            checks.append('one_markdown_and_json_for_entire_investigation')
            not_catalog=client.call('cortex_test_catalog_save',
                id=second_id,mutation_permission=False)
            assert not_catalog['isError'],not_catalog
            catalog_saved=client.data('cortex_test_catalog_save',
                id=second_id,mutation_permission=True)
            assert catalog_saved['entry']['requires_separate_permission'] and \
                catalog_saved['entry']['operator_approved_for_future_use'] is False
            entries=client.data('cortex_test_catalog_list')['entries']
            assert any(e['id']==second_id and
                e['candidate']['action']['steps']==second_proposal['action']['steps']
                for e in entries),entries
            checks.append('catalog_records_completed_scenario_without_approval')
            graph_field='Fixture.graph.counter'
            graph_event='Fixture.graph.input'
            field_record=client.data('project_knowledge_put',
                id=graph_field,kind='field',statement='Candidate numeric field',
                mutation_permission=True)
            assert field_record['result']['record']['status']=='hypothesis',field_record
            event_record=client.data('project_knowledge_put',
                id=graph_event,kind='event',statement='Candidate key event',
                relations=[{'target':graph_field,'type':'correlates',
                            'reference':fresh_action['id']}],
                mutation_permission=True)
            assert event_record['result']['record']['relations'][0]['target']==graph_field
            assert event_record['result']['record']['status']=='hypothesis'
            persisted_graph=client.data('project_knowledge_graph',
                focus=graph_event,depth=1,limit=4)
            persisted_nodes=persisted_graph['result']['nodes']
            persisted_edges=persisted_graph['result']['edges']
            assert {item['id'] for item in persisted_nodes}=={graph_event,graph_field}
            assert len(persisted_edges)==1 and \
                persisted_edges[0]['type']=='correlates' and \
                persisted_edges[0]['origin']=='client_assertion' and \
                persisted_edges[0]['causal_verified'] is False,persisted_graph
            checks.append('persisted_typed_knowledge_graph_not_causal_proof')
            host_graph=client.data('cortex_knowledge_graph',
                _cortex_target=pid,_cortex_generation=restarted['generation'],
                focus=graph_event,depth=1,limit=4)
            assert host_graph['schema']=='cortex.knowledge.graph.v1' and \
                len(host_graph['nodes'])==2 and len(host_graph['edges'])==1,host_graph
            checks.append('mcp_host_can_query_same_persistent_graph')
            limit=client.call('cortex_test_run',**base)
            assert limit['isError'] and limit['structuredContent']['error']['code']=='test_run_limit_reached',limit
            checks.append('operator_test_budget')
            exhausted_restart=client.call('cortex_restart',profile='fixture',pid=pid,
                                           generation=restarted['generation'],attach=True,mutation_permission=True)
            assert exhausted_restart['isError'] and exhausted_restart['structuredContent']['error']['code']=='launch_profile_run_limit_reached',exhausted_restart
            checks.append('restart_respects_run_budget')
            client.data('cortex_stop',profile='fixture',pid=pid,mutation_permission=True);pid=0
            client.close();client=None
            client=Mcp(exe,['--allow-state','--test-results',str(evidence)],root)
            archived=client.data('cortex_test_get',id=saved);assert archived['archived'] and archived['run']['outcome']=='passed';checks.append('report_reloaded_new_agent')
            generated_reopened=client.data('cortex_test_step_get',id=generated_step_id)
            assert generated_reopened['step']['status']=='awaiting_control' and \
                generated_reopened['step']['choice']['action']['steps'][0]['tap_ms']==80
            checks.append('generated_step_survives_agent_reconnect_without_replay')
            combined_reloaded=client.data('cortex_test_report_get',id=report_id)
            assert combined_reloaded['report']['id']==report_id and combined_reloaded['untrusted_data']
            assert combined_reloaded['report']['hypothesis_tests']['summary']['contradicted']==1
            checks.append('prediction_evidence_survives_reconnect')
            chosen_again=client.data('cortex_test_choose',report_id=unresolved_id,
                                     candidates=[repeat,click_probe])
            assert chosen_again['choice']['id']=='click_probe',chosen_again
            checks.append('next_experiment_recovered_after_reconnect')
            resumed_step=client.data('cortex_test_step_get',id=step_id)
            assert resumed_step['step']['status']=='completed' and \
                resumed_step['step']['action_id']==step_action['id'] and \
                resumed_step['hypothesis_tests']['summary']['contradicted']==1,resumed_step
            checks.append('supervised_step_recovered_after_reconnect')
            root_step=client.data('cortex_test_step_get',id=step_id)
            child_step=client.data('cortex_test_step_get',id=second_id)
            assert root_step['step']['next_step_id']==second_id and \
                child_step['step']['parent_step_id']==step_id and \
                child_step['step']['status']=='completed' and \
                child_step['hypothesis_tests']['summary']['contradicted']==1,(root_step,child_step)
            checks.append('two_round_investigation_reopens_after_reconnect')
            reopened_summary=client.data('cortex_test_investigation_summary',id=second_id)
            reopened_catalog=client.data('cortex_test_catalog_list')
            assert reopened_summary['round_count']==2 and \
                any(e['id']==second_id for e in reopened_catalog['entries'])
            checks.append('complete_report_and_catalog_survive_reconnection')
            checks.append('final_report_reloaded_new_agent')
            assert len(client.data('cortex_test_list')['runs'])>=5
        finally:
            if pid:subprocess.run(['taskkill','/PID',str(pid),'/F'],capture_output=True)
            if client:client.close()
    (evidence/'e2e-summary.json').write_text(json.dumps({'architecture':a.arch,'passed':checks,'count':len(checks)},indent=2))
    print('PASS Windows '+a.arch+': '+str(len(checks))+' checks: '+', '.join(checks))
if __name__=='__main__':main()
