"""Create an inert MSI DATABASE with the Windows SDK API (ctypes only).

Only database/view/record authoring APIs are called. There is no install,
product registration, sequence execution or custom-action invocation.
Cortex's parser uses no Windows Installer API; this is an independent producer.
"""
from pathlib import Path
import ctypes as c
import os


def main():
    if os.name!='nt': raise SystemExit('Fixture generation requires Windows; committed MSI is portable input.')
    here=Path(__file__).resolve().parent
    path=here/'microsoft.msi'
    if path.exists():
        print('Keeping existing fixture',path.name);return
    dll=c.WinDLL('msi');H=c.c_uint32;P=c.POINTER(H)
    def api(name,args,ret=c.c_uint32):
        fn=getattr(dll,name);fn.argtypes=args;fn.restype=ret;return fn
    open_db=api('MsiOpenDatabaseW',[c.c_wchar_p,c.c_void_p,P])
    open_view=api('MsiDatabaseOpenViewW',[H,c.c_wchar_p,P])
    execute=api('MsiViewExecute',[H,H]);commit=api('MsiDatabaseCommit',[H]);close=api('MsiCloseHandle',[H])
    create_record=api('MsiCreateRecord',[c.c_uint32],H)
    set_text=api('MsiRecordSetStringW',[H,c.c_uint32,c.c_wchar_p])
    set_stream=api('MsiRecordSetStreamW',[H,c.c_uint32,c.c_wchar_p])
    def ok(code,what):
        if code: raise RuntimeError('%s failed: %d'%(what,code))
    db=H();ok(open_db(str(path),c.c_void_p(3),c.byref(db)),'create database')
    try:
        def query(sql,record=0):
            view=H();ok(open_view(db,sql,c.byref(view)),sql)
            try:ok(execute(view,record),'execute SQL: '+sql)
            finally:close(view)
        query('CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` CHAR(0) LOCALIZABLE PRIMARY KEY `Property`)')
        query("INSERT INTO `Property` (`Property`,`Value`) VALUES ('ProductName','Cortex inert fixture')")
        query("INSERT INTO `Property` (`Property`,`Value`) VALUES ('ProductVersion','1.2.3')")
        query('CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, `Source` CHAR(72), `Target` CHAR(0) PRIMARY KEY `Action`)')
        query("INSERT INTO `CustomAction` (`Action`,`Type`,`Source`,`Target`) VALUES ('NeverRun',51,'CORTEX_TEST','THIS_IS_DATA_ONLY')")
        query('CREATE TABLE `Numbers` (`ID` SHORT NOT NULL, `Value` LONG, `Label` CHAR(72) PRIMARY KEY `ID`)')
        query("INSERT INTO `Numbers` (`ID`,`Value`,`Label`) VALUES (1,-123456,'negative')")
        query("INSERT INTO `Numbers` (`ID`,`Value`,`Label`) VALUES (2,70000,'positive')")
        query("INSERT INTO `Numbers` (`ID`,`Label`) VALUES (3,'null')")
        query('CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `DiskPrompt` CHAR(64), `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), `Source` CHAR(72) PRIMARY KEY `DiskId`)')
        query("INSERT INTO `Media` (`DiskId`,`LastSequence`,`Cabinet`) VALUES (1,2,'#payload.cab')")
        cabinet=here.parent/'cab'/'mszip-history.cab'
        if not cabinet.exists():raise RuntimeError('Run ../cab/make_fixture.py first')
        rec=create_record(2)
        try:
            ok(set_text(rec,1,'payload.cab'),'stream name')
            ok(set_stream(rec,2,str(cabinet)),'embed owned CAB')
            query('INSERT INTO `_Streams` (`Name`,`Data`) VALUES (?,?)',rec)
        finally:close(rec)
        ok(commit(db),'commit database')
    finally:close(db)
    print(path.name,path.stat().st_size)


if __name__=='__main__':main()
