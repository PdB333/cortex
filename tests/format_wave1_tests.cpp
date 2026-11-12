#include "pdb_tests.h"
#include "elf_tests.h"
#include "cab_tests.h"
#include "compound_tests.h"
#include "appx_tests.h"
#include <iostream>

int main(int argc,char** argv){
    const std::string fixtures=argc>1?argv[1]:"tests/fixtures";
    int failures=0;
    failures+=pdb_tests::Run(fixtures);
    failures+=elf_tests::Run(fixtures);
    failures+=cab_tests::Run(fixtures);
    failures+=compound_tests::Run(fixtures);
    failures+=appx_tests::Run(fixtures);
    if(failures){std::cerr<<failures<<" wave-1 assertion(s) failed\n";return 1;}
    std::cout<<"PDB, ELF, CAB, CFB/MSI and APPX/MSIX tests passed\n";
    return 0;
}
