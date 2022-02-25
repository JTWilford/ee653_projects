/*
 * Copyright 2002-2020 Intel Corporation.
 * 
 * This software is provided to you as Sample Source Code as defined in the accompanying
 * End User License Agreement for the Intel(R) Software Development Products ("Agreement")
 * section 1.L.
 * 
 * This software and the related documents are provided as is, with no express or implied
 * warranties, other than those that are expressly stated in the License.
 */

#include <iostream>
#include <fstream>
#include <stdlib.h>
#include "pin.H"
using std::cerr;
using std::cout;
using std::ofstream;
using std::ifstream;
using std::ios;
using std::string;
using std::endl;

static const UINT64 STORE_RESOLVE_CYCLES = 10;
static const UINT64 IBQ_SIZE = 64;
static const UINT64 MDPT_SIZE = 64;
static const UINT64 MDST_SIZE = 64;

ofstream OutFile;
// The running count of instructions is kept here
// make it static to help the compiler optimize docount
static UINT64 cycles = 0;
static UINT64 input = 0;
static UINT64 ld_ins_count = 0;
static UINT64 st_ins_count = 0;
static UINT64 predictions = 0;
static UINT64 mispredictions = 0;
static UINT64 speculations = 0;
static UINT64 mis_speculations = 0;
static UINT64 false_deps = 0;
static UINT64 ldst_buffer_time = 0;

static UINT64 uncommitted_stores = 0;   // Keeps track of the number of uncommitted stores in IBQ

struct IBQ_entry {
    ADDRINT addr;
    bool st;
    bool ld;
    bool speculative;
    bool committed;
    ADDRINT ea;
};

static IBQ_entry* IBQ = 0;
static UINT64 IBQ_tail = 0;
static UINT64 IBQ_count = 0;

struct MDPT_entry {
    bool valid;     // Valid flag
    UINT64 ldpc;    // Load PC
    UINT64 stpc;    // Store PC
    UINT64 dist;    // Dependency distance
    UINT64 pred;    // 2-bit up/down predictor
    UINT64 last_access;     // Tracks last access cycle for LRU replacement strategy
};
static MDPT_entry* MDPT = 0;

struct MDST_entry {
    bool valid;     // Valid flag
    UINT64 ldpc;    // Load PC
    UINT64 stpc;    // Store PC
    UINT64 ldid;    // Load ID
    UINT64 stid;    // Store ID
    UINT64 fe;      // Full/Empty flag
};
static MDST_entry* MDST = 0;

VOID Init() {
    // Initialize IBQ
    cout << "Initializing IBQ" << endl;
    IBQ = (IBQ_entry*) malloc(sizeof(IBQ_entry) * IBQ_SIZE);
    cout << "IBQ Pointer: " << IBQ << endl;
    // Initialize MDPT
    MDPT = (MDPT_entry*) malloc(sizeof(MDPT_entry) * MDPT_SIZE);
    for(UINT64 i = 0; i < MDPT_SIZE; i++) {
        MDPT_entry mdpt;
        mdpt.valid = false;
        MDPT[i] = mdpt;
    }
    // Initialize MDST
    MDST = (MDST_entry*) malloc(sizeof(MDST_entry) * MDST_SIZE);
    for(UINT64 i = 0; i < MDST_SIZE; i++) {
        MDST_entry mdst;
        mdst.valid = false;
        MDST[i] = mdst;
    }
}

VOID Release() {
    free(MDST);
    free(MDPT);
    free(IBQ);
}

// Allocates the next available MDPT entry
UINT64 allocateNewMDPTEntry() {
    // Search for the first invalid entry, or the lease recently used entry
    UINT64 lru = 0;
    UINT64 lru_last_access = MDPT[0].last_access;
    for (UINT64 i = 0; i < MDPT_SIZE; i++) {
        // Invalid entry found. Return its index
        if (!MDPT[i].valid)
            return(i);
        if (MDPT[i].last_access < lru_last_access) {
            lru_last_access = MDPT[i].last_access;
            lru = i;
        }
    }
    // No invalid entries found. Return the least recently used entry
    return (lru);
}

// This function is called before every instruction is executed
VOID docount(ADDRINT ins_addr, bool ins_st, bool ins_ld, ADDRINT ea) {
    cycles++;

    // Check to see if previous stores have been resolved
    // We're starting from the top of the IBQ rather than the bottom
    // so we can chain resolve instructions (ie. if a store resolves,
    // and a junior load was waiting, we can resolve both in one loop)
    // cout << "Cycle: " << cycles << endl;

    // We only check if its been STORE_RESOLVE_CYCLES since a store has entered IBQ
    if (IBQ_count >= STORE_RESOLVE_CYCLES) {
        int index = IBQ_tail - STORE_RESOLVE_CYCLES;
        if (index < 0) {
            index = index + IBQ_SIZE;
        }
        // Look for uncommitted store
        if (IBQ[index].st && !IBQ[index].committed) {
            // Should be committed
            IBQ[index].committed = true;
            uncommitted_stores--;
            // Update the MDST as well
            for (UINT64 j = 0; j < MDST_SIZE; j++) {
                if (MDST[j].valid && MDST[j].stpc == IBQ[index].addr && MDST[j].stid == (UINT64) index) {
                    // Mark the entry as complete
                    MDST[j].fe = true;
                    
                    cout << "Res MDST: " << j << ": L" << MDST[j].ldpc << " S" << MDST[j].stpc << " DL" << MDST[j].ldid << " DS" << MDST[j].stid << endl;
                    // Commit the corresponding Load (if it exists)
                    UINT64 ldid = MDST[j].ldid;
                    // Make sure the entry was actually used
                    if (ldid < IBQ_SIZE && IBQ[ldid].ld && IBQ[ldid].addr == MDST[j].ldpc) {
                        // Find the corresponding MDPT table entry for updating
                        UINT64 k = 0;
                        for (; k < MDPT_SIZE; k++)
                            if (MDPT[k].valid && MDPT[k].ldpc == IBQ[ldid].addr && MDPT[k].stpc == IBQ[index].addr)
                                break;
                        MDPT[k].last_access = cycles;
                        // Check whether it was a true dependency, and whether the prediction was correct
                        if (IBQ[ldid].ea == IBQ[index].ea) {
                            // True dependecy found. Update MDPT and Check for misprediction
                            if (MDPT[k].pred < 3)
                                MDPT[k].pred++;
                            cout << "Prd MDPT: " << k << ": L" << MDPT[k].ldpc << " S" << MDPT[k].stpc << " D" << MDPT[k].dist << " P" << MDPT[k].pred << endl;
                            if (IBQ[ldid].speculative) {
                                // MDPT Misprediction and Mis-speculation
                                mispredictions++;
                                mis_speculations++;
                                // Put the Load back through the LSQ (1 cycle penalty)
                                ldst_buffer_time++;
                                IBQ[ldid].speculative = false;
                                IBQ[ldid].committed = true;
                            }
                            else {
                                // Commit the Load
                                IBQ[ldid].committed = true;
                                // Add the Load's waiting time in the LSQ
                                int time = IBQ_tail - ldid;
                                if (time < 0) {
                                    time += IBQ_SIZE;
                                }
                                ldst_buffer_time += time;
                            }
                        }
                        else {
                            // False dependency found. Update MDPT and check for misprediction
                            if (MDPT[k].pred > 0)
                                MDPT[k].pred--;
                            cout << "Prd MDPT: " << k << ": L" << MDPT[k].ldpc << " S" << MDPT[k].stpc << " D" << MDPT[k].dist << " P" << MDPT[k].pred << endl;
                            if (!IBQ[ldid].speculative) {
                                mispredictions++;
                                false_deps++;
                                // Commit the Load
                                IBQ[ldid].committed = true;
                                // Add the Load's waiting time in the LSQ
                                int time = IBQ_tail - ldid;
                                if (time < 0) {
                                    time += IBQ_SIZE;
                                }
                                ldst_buffer_time += time;
                            }
                            else {
                                // If it was speculative, just mark it as committed
                                IBQ[ldid].speculative = false;
                                IBQ[ldid].committed = true;
                            }
                        }
                    }
                }
            }
            // Now we need to walk through the last STORE_RESOLVE_CYCLES IBQ entries to find uncaught Load conflicts
            // NOTE: We are doing this step by walking the IBQ because we don't have a CDB in this simulation
            for (UINT64 j = 1; j <= STORE_RESOLVE_CYCLES; j++) {
                int jindex = IBQ_tail - j;
                if (jindex < 0) {
                    jindex = jindex + IBQ_SIZE;
                }
                if (IBQ[jindex].speculative && IBQ[jindex].ea == IBQ[index].ea) {
                    // Found a Load conflict without an MDPT entry
                    // Mis-speculation
                    mis_speculations++;
                    // Commit the Load
                    IBQ[jindex].committed = true;
                    IBQ[jindex].speculative = false;
                    ldst_buffer_time++;
                    // Make a new MDPT entry
                    int diff = jindex - index;
                    if (diff < 0)
                        diff = diff + IBQ_SIZE;
                    MDPT_entry mdpt;
                    mdpt.valid = true;
                    mdpt.ldpc = IBQ[jindex].addr;
                    mdpt.stpc = IBQ[index].addr;
                    mdpt.dist = diff;
                    mdpt.pred = 1;
                    mdpt.last_access = cycles;
                    UINT64 mdpt_index = allocateNewMDPTEntry();
                    MDPT[mdpt_index] = mdpt;

                    cout << "New MDPT: " << mdpt_index << ": L" << MDPT[mdpt_index].ldpc << " S" << MDPT[mdpt_index].stpc << " D" << MDPT[mdpt_index].dist << " P" << MDPT[mdpt_index].pred << endl;
                }
            }
        }
    }
    // Done with committing stores

    // Create a new IBQ entry
    IBQ_entry ibq;
    ibq.addr = ins_addr;
    ibq.st = ins_st;
    ibq.ld = ins_ld;
    ibq.ea = ea;
    
    // If the instruction was a load, we have to search for potential store dependencies in MDPT
    if (ins_ld) {
        ld_ins_count++;
        // Walk throught the MDPT, looking for historic store conflicts
        UINT64 i = 0;
        for (; i < MDPT_SIZE; i++)
            if (MDPT[i].valid && MDPT[i].ldpc == ins_addr)
                break;
        if (i < MDPT_SIZE) {
            // Find the corresponding MDST entry
            int st_index = IBQ_tail - MDPT[i].dist;
            if (st_index < 0)
                st_index = st_index + IBQ_SIZE;
            UINT64 j = 0;
            for (; j < MDST_SIZE; j++)
                if (MDST[j].valid && MDST[j].ldpc == MDPT[i].ldpc && MDST[j].stpc == MDPT[i].stpc && MDST[j].stid == (UINT64) st_index)
                    break;
            // Found the entry. Add the load ID
            MDST[j].ldid = IBQ_tail;
            cout << "Udt MDST: " << j << ": L" << MDST[j].ldpc << " S" << MDST[j].stpc << " DL" << MDST[j].ldid << " DS" << MDST[j].stid << endl;

            // If the store has already committed, then no need to predict
            if (MDST[j].fe) {
                ibq.speculative = false;
                ibq.committed = true;
                ldst_buffer_time++;
            }
            // Predict whether there is a dependency
            else {
                predictions++;
                if (MDPT[i].pred < 2) {
                    // Predict no dependency (speculate)
                    speculations++;
                    ibq.speculative = true;
                    ibq.committed = false;
                    ldst_buffer_time++;
                }
                else {
                    // Predict dependency
                    ibq.speculative = false;
                    ibq.committed = false;
                }
            }
        }
        else {
            // No MDPT entry found. We always predict no dependency here
            // If there are uncommitted stores in the IBQ, then we speculate
            if (uncommitted_stores > 0) {
                speculations++;
                ibq.speculative = true;
                ibq.committed = false;
                ldst_buffer_time++;
            }
            else {
                // No uncommitted stores, so this load is fully committed (no speculation)
                ibq.speculative = false;
                ibq.committed = true;
                ldst_buffer_time++;
            }
        }
    }

    // If the instruction is a store, we have to search for potential load dependencies in MDPT
    if (ins_st) {
        st_ins_count++;
        uncommitted_stores++;
        ibq.committed = false;
        ibq.speculative = false;
        // Walk through the MDPT, looking for previous load conflicts
        for (UINT64 i = 0; i < MDPT_SIZE; i++) {
            if (MDPT[i].valid && MDPT[i].stpc == ins_addr) {
                // Found a previous conflict. Make a new MDST entry
                MDST_entry mdst;
                mdst.valid = true;
                mdst.ldpc = MDPT[i].ldpc;
                mdst.stpc = MDPT[i].stpc;
                mdst.ldid = IBQ_SIZE;
                mdst.stid = IBQ_tail;
                mdst.fe = false;

                // Insert into the MDST wherever there's an invalid entry
                for (UINT64 j = 0; j < MDST_SIZE; j++) {
                    if (!MDST[j].valid) {
                        MDST[j] = mdst;
                        cout << "New MDST: " << j << ": L" << MDST[j].ldpc << " S" << MDST[j].stpc << " DL" << MDST[j].ldid << " DS" << MDST[j].stid << endl;
                        break;
                    }
                }
            }
        }
    }

    // Retire the top most instruction in the IBQ (if the IBQ is full)
    if (IBQ_count >= IBQ_SIZE) {
        // If its a store, then we need to remove its MDST entries
        if (IBQ[IBQ_tail].st) {
            // Walk through MDST to invalidate all this Store's entries
            UINT64 j = 0;
            for (; j < MDST_SIZE; j++) {
                if (MDST[j].valid && MDST[j].stpc == IBQ[IBQ_tail].addr && MDST[j].stid == IBQ_tail) {
                    MDST[j].valid = false;
                }
            }
        }
    }

    // Add the new instruction to the IBQ
    IBQ[IBQ_tail] = ibq;
    IBQ_tail++;
    if (IBQ_tail >= IBQ_SIZE)
        IBQ_tail = 0;
    if (IBQ_count < IBQ_SIZE)
        IBQ_count++;
}
    
// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
    // Insert a call to docount before every instruction
    // If the instruction is a load or store, send the Effective Address as well
    if (INS_IsMemoryRead(ins))
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)docount,
        IARG_INST_PTR,          // Instruction Address
        IARG_BOOL, true,        // Is a store?
        IARG_BOOL, false,       // Is a load?
        IARG_MEMORYREAD_EA,     // Memory Effective Address
        IARG_END);
    else if (INS_IsMemoryWrite(ins))
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)docount,
        IARG_INST_PTR,          // Instruction Address
        IARG_BOOL, false,       // Is a store?
        IARG_BOOL, true,        // Is a load?
        IARG_MEMORYWRITE_EA,    // Memory Effective Address
        IARG_END);
    else
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)docount,
        IARG_INST_PTR,          // Instruction Address
        IARG_BOOL, false,       // Is a store?
        IARG_BOOL, false,       // Is a load?
        IARG_ADDRINT, 0,        // Memory Effective Address
        IARG_END);
}

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "loadStore.out", "specify output file name");

// This function is called when the application exits
VOID Fini(INT32 code, VOID *v)
{
    // Write to a file since cout and cerr maybe closed by the application
    OutFile.setf(ios::showbase);
    OutFile << "Total Instructions: " << cycles << endl;
    OutFile << "Total Loads: " << ld_ins_count << endl;
    OutFile << "Total Stores: " << st_ins_count << endl;
    OutFile << "Total MDPT Predictions: " << predictions << endl;
    OutFile << "Total MDPT Mispredictions: " << mispredictions << endl;
    OutFile << "Misprediction Rate: " << (double)mispredictions / (double)predictions << endl;
    OutFile << "Total Load Speculations: " << speculations << endl;
    OutFile << "Total Load Mis-speculations: " << mis_speculations << endl;
    OutFile << "Mis-speculation Rate: " << (double)mis_speculations / (double)speculations << endl;
    OutFile << "Total False Dependencies: "<< false_deps << endl;
    OutFile << "Mis-speculations due to False Dependencies: " << (double)false_deps / (double)mis_speculations << endl;
    OutFile << "Avg. Time in LD/ST Buffer (Loads): " << (double) ldst_buffer_time / (double) ld_ins_count << endl;
    OutFile.close();

    Release();
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    cerr << "This tool counts the number of dynamic instructions executed" << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */
/*   argc, argv are the entire command line: pin -t <toolname> -- ...    */
/* ===================================================================== */

int main(int argc, char * argv[])
{
    // Initialize data structures
    Init();
    // Initialize pin
    if (PIN_Init(argc, argv)) return Usage();
    OutFile.open(KnobOutputFile.Value().c_str());
    ifstream InputFile("input.txt");

    InputFile >> input;
    
    //cout << "Here was the input file:" << endl << input << endl;

    // Register Instruction to be called to instrument instructions
    INS_AddInstrumentFunction(Instruction, 0);

    // Register Fini to be called when the application exits
    PIN_AddFiniFunction(Fini, 0);
    
    // Start the program, never returns
    PIN_StartProgram();
    
    return 0;
}
