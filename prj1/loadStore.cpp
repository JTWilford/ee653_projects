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
static double avg_ldst_buffer_time = 0.0;

struct IBQ_entry {
    ADDRINT addr;
    bool st;
    bool ld;
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
    cout << "Cycle: " << cycles << endl;
    bool committed_st = false;
    int committed_st_index = 0;
    if (IBQ_count) {
        for (UINT64 i = IBQ_count-1; i != 0; i--) {
            int index = IBQ_tail - 1 - i;
            if (index < 0) {
                index = index + IBQ_SIZE;
            }
            // Look for loads and stores
            // First case: uncommitted store. Its committed after a number of cycles of IBQ time
            if (IBQ[index].st && !IBQ[index].committed && i >= STORE_RESOLVE_CYCLES) {
                // Should be committed
                IBQ[index].committed = true;
                // Update the MDST as well
                for (UINT64 j = 0; j < MDST_SIZE; j++) {
                    if (MDST[j].valid && MDST[j].stpc == IBQ[index].addr && MDST[j].stid == (UINT64) index)
                        // Mark the entry as complete
                        MDST[j].fe = true;
                }
                // Key observatoin: Only one store should ever commit in this loop (since its executed every cycle)
                // Makes determining mis-speculations really easy
                committed_st = true;
                committed_st_index = index;
            }
            // Second case: its a committed load. Need to check for a mis-speculations
            else if (IBQ[index].ld && IBQ[index].committed && committed_st) {
                // Check for mis-speculations
                if (IBQ[index].ea == IBQ[committed_st_index].ea) {
                    // Mis-speculation detected
                    mis_speculations++;
                    mispredictions++;
                    
                    // Update average load store buffer time (assume loads always spend at least 1 cycle in lsb)
                    avg_ldst_buffer_time += (1.0 / (double)ld_ins_count);

                    // Look for an MDPT entry to update
                    UINT64 j = 0;
                    for (; j < MDPT_SIZE; j++)
                        if (MDPT[j].valid && MDPT[j].ldpc == IBQ[index].addr && MDPT[j].stpc == IBQ[committed_st_index].addr)
                            break;
                    if (j < MDPT_SIZE) {
                        // MDPT entry found. Increment it because a true dependency was found
                        MDPT[j].pred++;
                    }
                    else {
                        // No MDPT entry found. Make a new one
                        int diff = index - committed_st_index;
                        if (diff < 0)
                            diff = diff + IBQ_SIZE;
                        MDPT_entry mdpt;
                        mdpt.valid = true;
                        mdpt.ldpc = IBQ[index].addr;
                        mdpt.stpc = IBQ[committed_st_index].addr;
                        mdpt.dist = diff;
                        mdpt.pred = 1;
                        mdpt.last_access = cycles;
                        UINT64 mdpt_index = allocateNewMDPTEntry();
                        MDPT[mdpt_index] = mdpt;
                    }
                }
            }
            else if (IBQ[index].ld && !IBQ[index].committed) {
                // Check MDST to see if the dependency was resolved
                for (UINT64 j = 0; j < MDST_SIZE; j++) {
                    if (MDST[j].valid && MDST[j].ldpc == IBQ[index].addr && MDST[j].ldid == (UINT64) index && MDST[j].fe) {
                        // Dependency was resolved.
                        IBQ[index].committed = true;
                        // Calculate time spent in LDST Buffer (statistics)
                        int time = IBQ_tail - index;
                        if (time < 0)
                            time = -1 * time;
                        avg_ldst_buffer_time += ((double)time / (double)ld_ins_count);
                        // Check to see if it was a true dependency and update MDPT
                        // Find MDPT entry
                        UINT64 k = 0;
                        for (; k < MDPT_SIZE; k++)
                            if (MDPT[k].valid && MDPT[k].ldpc == IBQ[index].addr && MDPT[k].stpc == MDST[j].stpc)
                                break;
                        
                        // True dependency. Need to increment MDPT counter
                        if (IBQ[MDST[j].stid].ea == IBQ[index].ea) {
                            if (MDPT[k].pred != 3)
                                MDPT[k].pred++;
                        }
                        // False dependency. Need to decrement MDPT counter and record a misprediction
                        else {
                            if (MDPT[k].pred != 0) {
                                MDPT[k].pred--;
                                false_deps++;
                                mispredictions++;
                            }
                        }
                        // Set MDPT last access time
                        MDPT[k].last_access = cycles;
                        // Invalidate MDST entry
                        MDST[j].valid = false;
                    }
                }
            }
        }
    }

    // Create a new IBQ entry
    IBQ_entry ibq;
    ibq.addr = ins_addr;
    ibq.st = ins_st;
    ibq.ld = ins_ld;
    ibq.ea = ea;
    // If the instruction is a load or a store, it won't be committed yet
    ibq.committed = !(ins_st || ins_ld);
    
    // If the instruction was a load, we have to search for potential store dependencies in MDPT
    if (ins_ld) {
        ld_ins_count++;
        // Walk throught the MDPT, looking for historic store conflicts
        UINT64 i = 0;
        for (; i < MDPT_SIZE; i++)
            if (MDPT[i].valid && MDPT[i].ldpc == ins_addr)
                break;
        if (i < MDPT_SIZE) {
            // Found an MDPT entry. Predict whether there is a dependency
            // We mark the instruction as committed if we predict no dependency
            ibq.committed = (MDPT[i].pred < 2);
            predictions++;
            // If committed, then this was a speculation (statistics)
            if (ibq.committed) {
                speculations++;
                // Update average load store buffer time (assume loads always spend at least 1 cycle in lsb)
                avg_ldst_buffer_time += (1.0 / (double)ld_ins_count);
            }
        }
        else {
            // No MDPT entry found. We always predict no dependency here
            ibq.committed = true;
            // Update average load store buffer time (assume loads always spend at least 1 cycle in lsb)
            avg_ldst_buffer_time += (1.0 / (double)ld_ins_count);
            // Check for stores within previous resolve cycle instructions (this is for statistics only)
            // This determines whether the load was speculative or not
            for (UINT64 k = 0; k < STORE_RESOLVE_CYCLES; k++) {
                int index = IBQ_tail - k;
                if (index < 0)
                    index = index + IBQ_tail;
                if (IBQ[index].st) {
                    predictions++;
                    speculations++;
                    break;
                }
            }
        }

        // If we predict dependency, we need to update the MDST entry
        if (!ibq.committed) {
            // Walk through the MDST to find the corresponding entry (should have been made when store entered IBQ)
            int st_index = IBQ_tail - MDPT[i].dist;
            if (st_index < 0)
                st_index = st_index + IBQ_SIZE;
            UINT64 j = 0;
            for (; j < MDST_SIZE; j++)
                if (MDST[j].valid && MDST[j].ldpc == MDPT[i].ldpc && MDST[j].stpc == MDPT[i].stpc && MDST[j].stid == (UINT64) st_index)
                    break;
            // Found the entry. Add the load ID
            MDST[j].ldid = IBQ_tail;
        }
    }
    // If the instruction is a store, we have to search for potential load dependencies in MDPT
    
    if (ins_st) {
        st_ins_count++;
        // Walk through the MDPT, looking for previous load conflicts
        for (UINT64 i = 0; i < MDPT_SIZE; i++) {
            if (MDPT[i].valid && MDPT[i].stpc == ins_addr && MDPT[i].pred >= 2) {
                // Found a predicted conflict. Make a new MDST entry
                MDST_entry mdst;
                mdst.valid = true;
                mdst.ldpc = MDPT[i].ldpc;
                mdst.stpc = MDPT[i].stpc;
                mdst.ldid = IBQ_SIZE;
                mdst.stid = IBQ_tail;
                mdst.fe = false;
                // Insert into the MDST wherever there's an invalid entry
                for (UINT64 j = 0; j < MDST_SIZE; j++)
                    if (!MDST[j].valid)
                        MDST[j] = mdst;
            }
        }
    }

    // Add the new instruction to the IBQ
    IBQ[IBQ_tail++] = ibq;
    if (IBQ_tail >= IBQ_SIZE)
        IBQ_tail = 0;
    if (IBQ_count < IBQ_SIZE-1)
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
    OutFile << "Total Predictions: " << predictions << endl;
    OutFile << "Total Mispredictions: " << mispredictions << endl;
    OutFile << "Misprediction Rate: " << (double)mispredictions / (double)predictions << endl;
    OutFile << "Total Speculations: " << speculations << endl;
    OutFile << "Total Mis-speculations: " << mis_speculations << endl;
    OutFile << "Mis-speculation Rate: " << (double)mis_speculations / (double)speculations << endl;
    OutFile << "Total False Dependencies: "<< false_deps << endl;
    OutFile << "Mis-speculations due to False Dependencies: " << (double)false_deps / (double)mis_speculations << endl;
    OutFile << "Avg. Time in LD/ST Buffer (Loads): " << avg_ldst_buffer_time << endl;
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
