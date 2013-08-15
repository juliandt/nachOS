// addrspace.cc 
//    Routines to manage address spaces (executing user programs).
//
//    In order to run a user program, you must:
//
//    1. link with the -N -T 0 option 
//    2. run coff2noff to convert the object file to Nachos format
//        (Nachos object code format is essentially just a simpler
//        version of the UNIX executable object code format)
//    3. load the NOFF file into the Nachos file system
//        (if you haven't implemented the file system yet, you
//        don't need to do this last step)
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "system.h"
#include "addrspace.h"
#include "syscall_utils.h"

//----------------------------------------------------------------------
// SwapHeader
//    Do little endian to big endian conversion on the bytes in the 
//    object file header, in case the file was generated on a little
//    endian machine, and we're now running on a big endian machine.
//----------------------------------------------------------------------

static void 
SwapHeader (NoffHeader *noffH)
{
    noffH->noffMagic = WordToHost(noffH->noffMagic);
    noffH->code.size = WordToHost(noffH->code.size);
    noffH->code.virtualAddr = WordToHost(noffH->code.virtualAddr);
    noffH->code.inFileAddr = WordToHost(noffH->code.inFileAddr);
    noffH->initData.size = WordToHost(noffH->initData.size);
    noffH->initData.virtualAddr = WordToHost(noffH->initData.virtualAddr);
    noffH->initData.inFileAddr = WordToHost(noffH->initData.inFileAddr);
    noffH->uninitData.size = WordToHost(noffH->uninitData.size);
    noffH->uninitData.virtualAddr = WordToHost(noffH->uninitData.virtualAddr);
    noffH->uninitData.inFileAddr = WordToHost(noffH->uninitData.inFileAddr);
}

//----------------------------------------------------------------------
// AddrSpace::AddrSpace
//    Create an address space to run a user program.
//    Load the program from a file "executable", and set everything
//    up so that we can start executing user instructions.
//
//    Assumes that the object code file is in NOFF format.
//
//    First, set up the translation from program memory to physical 
//    memory.  For now, this is really simple (1:1), since we are
//    only uniprogramming, and we have a single unsegmented page table
//
//    "executable" is the file containing the object code to load into memory
//----------------------------------------------------------------------

AddrSpace::AddrSpace(OpenFile *executable)
{
    memoryBitMap = MemoryBitMap::GetInstance();
    unsigned int i, size;

    this->executable = executable;
    executable->ReadAt((char *)&noffH, sizeof(noffH), 0);
    if ((noffH.noffMagic != NOFFMAGIC) && 
        (WordToHost(noffH.noffMagic) == NOFFMAGIC))
        SwapHeader(&noffH);
    ASSERT(noffH.noffMagic == NOFFMAGIC);

    // how big is address space?
    size = noffH.code.size + noffH.initData.size + noffH.uninitData.size 
            + UserStackSize;    // we need to increase the size
                                // to leave room for the stack
    numPages = divRoundUp(size, PageSize);
    size = numPages * PageSize;

    ASSERT(numPages <= (unsigned int)memoryBitMap->NumClear());    
                                                // check we're not trying
                                                // to run anything too big --
                                                // at least until we have
                                                // virtual memory

    pageTable = new TranslationEntry[numPages];
#ifdef USE_TLB
    last_modify = 0;
    DEBUG('a', "Creating pageTable, num pages %d,not loading anything\n", numPages);
    for (i = 0; i < numPages; i++) {
        pageTable[i].virtualPage = i;    // The translation is not valid
        pageTable[i].physicalPage = -1;  // assign 0 to show that
        pageTable[i].valid = false;
        pageTable[i].use = false;
        pageTable[i].dirty = false;
        pageTable[i].readOnly = false; 
    }
#else
    DEBUG('a', "Initializing address space, num pages %d, size %d\n", 
                    numPages, size);
    // first, set up the translation 
    for (i = 0; i < numPages; i++) {
        pageTable[i].virtualPage = i;    
        pageTable[i].physicalPage = memoryBitMap->Find();
        pageTable[i].valid = true;
        pageTable[i].use = false;
        pageTable[i].dirty = false;
        pageTable[i].readOnly = false;  // if the code segment was entirely on 
                                        // a separate page, we could set its 
                                        // pages to be read-only
    }
    
    // zero out the entire address space, to zero the unitialized data segment 
    // and the stack segment
    for (i = 0; i < numPages; i++){
        bzero((machine->mainMemory) + (pageTable[i].physicalPage * PageSize), PageSize);
    }
    

    // then, copy in the code and data segments into memory
    if (noffH.code.size > 0) {
        DEBUG('a', "Initializing code segment, at 0x%x, size %d\n", 
            noffH.code.virtualAddr, noffH.code.size);
        CopyToMemory(noffH.code.virtualAddr,
                     noffH.code.inFileAddr,
                     noffH.code.size);
    }
    if (noffH.initData.size > 0) {
        DEBUG('a', "Initializing data segment, at 0x%x, size %d\n", 
            noffH.initData.virtualAddr, noffH.initData.size);
        CopyToMemory(noffH.initData.virtualAddr,
                     noffH.initData.inFileAddr,
                     noffH.initData.size);
    }
#endif

}

//----------------------------------------------------------------------
// AddrSpace::~AddrSpace
//     Dealloate an address space.  Nothing for now!
//----------------------------------------------------------------------

AddrSpace::~AddrSpace()
{

    int i;
    for(i = 0; i < (unsigned int) numPages ; i++){  
        memoryBitMap->Clear(pageTable[i].physicalPage);
    }
    delete [] pageTable;
    delete [] this->argv;
}

//----------------------------------------------------------------------
// AddrSpace::InitRegisters
//    Set the initial values for the user-level register set.
//
//    We write these directly into the "machine" registers, so
//    that we can immediately jump to user code.  Note that these
//    will be saved/restored into the currentThread->userRegisters
//    when this thread is context switched out.
//----------------------------------------------------------------------

void
AddrSpace::InitRegisters()
{
    int i;

    for (i = 0; i < NumTotalRegs; i++)
    machine->WriteRegister(i, 0);

    // Initial program counter -- must be location of "Start"
    machine->WriteRegister(PCReg, 0);    

    // Need to also tell MIPS where next instruction is, because
    // of branch delay possibility
    machine->WriteRegister(NextPCReg, 4);

    // Set the stack register to the end of the address space, where we
    // allocated the stack; but subtract off a bit, to make sure we don't
    // accidentally reference off the end!
    machine->WriteRegister(StackReg, numPages * PageSize - 16);

    DEBUG('a', "Initializing stack register to %d\n", numPages * PageSize - 16);
}

//----------------------------------------------------------------------
// AddrSpace::SaveState
//    On a context switch, save any machine state, specific
//    to this address space, that needs saving.
//
//    For now, nothing!
//----------------------------------------------------------------------

void AddrSpace::SaveState() 
{
    currentThread->SaveUserState();
}


//----------------------------------------------------------------------
// AddrSpace::RestoreState
//    On a context switch, restore the machine state so that
//    this address space can run.
//
//    For now, tell the machine where to find the page table.
//----------------------------------------------------------------------

void AddrSpace::RestoreState() {
#ifdef USE_TLB
    int i;
    for(i=0; i < TLBSize; i++){
        machine->tlb[i].valid = false;
    }
#else
    machine->pageTable = pageTable;
    machine->pageTableSize = numPages;
#endif
}


void AddrSpace::CopyToMemory(int virtualAddr, int inFileAddr, int size){
    int pageNbr;
    ASSERT(size >= 0);

    int i;
    int offset;
    for (i=0; i < size ; i++){
        // virtualAddr + i -> la posicion en memoria que quiero copiar
        // pageNbr * PageSize -> La direccion donde comienza la pagina a
        //                      la que pertenece lo anterior
        pageNbr = (virtualAddr + i) /  PageSize;
        offset = (virtualAddr + i) - (pageNbr * PageSize);
        executable->ReadAt(
                &(machine->mainMemory[pageTable[pageNbr].physicalPage * PageSize + offset]), 
                1, 
                inFileAddr + i); 
        DEBUG('a',"Loaded virtualAddr %d into physPage %d from position in file=%d\n", 
                virtualAddr + i,
                pageTable[pageNbr].physicalPage,
                inFileAddr + i);
    }

}

bool AddrSpace::isCode (int addr) {
    return (addr>= noffH.code.virtualAddr && addr< noffH.code.virtualAddr + noffH.code.size);
}

bool AddrSpace::isData (int addr) {
    return (addr>= noffH.initData.virtualAddr && addr< noffH.initData.virtualAddr + noffH.initData.size);
}

void AddrSpace::LoadPage(int badAddr){

    unsigned int virtPage;
    virtPage = badAddr / PageSize;

    int virtAddr = virtPage * PageSize;
    
    if(pageTable[virtPage].physicalPage == -1){
        pageTable[virtPage].physicalPage = memoryBitMap->Find();
        // Just in case...
        bzero((machine->mainMemory + pageTable[virtPage].physicalPage * PageSize), PageSize);
        pageTable[virtPage].valid = true;
        DEBUG('a',"Copying virtual page %d into physical page %d\n",virtPage, pageTable[virtPage].physicalPage );
        for(int i=0; i < PageSize; i++){
            if (isCode(virtAddr + i)){
                CopyToMemory(virtAddr + i, noffH.code.inFileAddr + (virtAddr - noffH.code.virtualAddr + i) , 1); 
            }
            if (isData(virtAddr + i)){
                CopyToMemory(virtAddr + i, noffH.initData.inFileAddr + (virtAddr - noffH.initData.virtualAddr + i) , 1); 
            }
        }
    }else{
        DEBUG('a',"Page %d was already loaded\n", virtPage);
    }
}

void AddrSpace::LoadArguments(){
    int args[argc];
    int strLen;

    int sp = numPages * PageSize;
    // Leo todos los argumentos de argv
    int cantPag = 0;
    int tmpSize = 4 * argc;
    
    for (int i = 0; i < argc; i++) {
        strLen = strlen(argv[i]) + 1;
        tmpSize += strLen;  
        // Pedimos las paginas antes de escribir en memoria, para evitarnos
        // tener que llamar a LazyCall que "saca" las cosas del executable
        if (tmpSize > PageSize * cantPag ){
            cantPag = cantPag + 1;
            pageTable[numPages - cantPag].physicalPage =  memoryBitMap->Find();
            // Just in case...
            pageTable[numPages - cantPag].valid =  true;
            bzero((machine->mainMemory) + (pageTable[numPages - cantPag].physicalPage * PageSize), PageSize);
        }
        sp = sp - strLen;
        writeString(sp, argv[i], strLen);
        args[i] = sp;
    }

    sp = sp - argc * 4;
    sp = sp - sp % 4;
    machine->WriteRegister(StackReg, sp - 4*4);
    machine->WriteRegister(4,argc);
    machine->WriteRegister(5,sp);
 
    for (int i = 0; i < argc; i++) {
        WRITEMEM(sp, 4, args[i]);
        sp = sp + 4;
    };
 
}

void AddrSpace::SetArguments(int argc, int argv, char* file_name){
    int arg_ptr;
    char tempStr[128];

    this->argc = argc + 1;
    // TODO ver como podemos hacer mas lindo esto
    //this->argv = (char **) new int[argc + 1];
    this->argv = (char **) malloc(this->argc * sizeof(char *));

    // Load file_name in argv[0]
    this->argv[0] = new char[strlen(file_name) + 1];
    strcpy(this->argv[0], file_name);
    
    // Leo todos los argumentos de argv
    for (int i = 0; i < argc; i++) {
        memset(tempStr, '\0', 128);
        READMEM(argv + 4*i, 4, &arg_ptr);
        readString(arg_ptr, tempStr);
        this->argv[i+1] = new char[strlen(tempStr) + 1];
        strcpy(this->argv[i+1], tempStr);
    }
}

void AddrSpace::UpdateTLB(){
    int badAddr;
    badAddr = machine->ReadRegister(BadVAddrReg);
    unsigned int virtPage;
    virtPage = badAddr / PageSize;
    LoadPage(badAddr);
    last_modify = last_modify % TLBSize;
    DEBUG('a',
      "Inserting into TLB in position %d\n\tpageTable.VirtPage=%d\n\tpageTable.physicalPage = %d\n",
      last_modify, virtPage, pageTable[virtPage].physicalPage);
    //memcpy(&machine->tlb[last_modify], &pageTable[virtPage], sizeof(TranslationEntry));
    //TranslationEntry *asd = pageTable[virtPage];
    //machine->tlb[last_modify] = asd;
    machine->tlb[last_modify] = pageTable[virtPage];
    last_modify = last_modify + 1;
}
