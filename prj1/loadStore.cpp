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
#include<stdlib.h>
#include "pin.H"
using std::cerr;
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
static UINT64 mispredictions = 0;
static UINT64 ld_ins_count = 0;
static UINT64 st_ins_count = 0;

struct IBQ_entry {
    INS ins;
    bool committed;
    ADDRINT ea;
};

static IBQ_entry* IBQ = 0;
static UINT64 IBQ_size = IBQ_SIZE;
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
    IBQ = (IBQ_entry*) malloc(sizeof(IBQ_entry) * IBQ_SIZE);
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
VOID docount(INS ins, ADDRINT ea) {
    cycles++;

    // Check to see if previous stores have been resolved
    // We're starting from the top of the IBQ rather than the bottom
    // so we can chain resolve instructions (ie. if a store resolves,
    // and a junior load was waiting, we can resolve both in one loop)
    bool committed_st = false;
    int committed_st_index = 0;
    for (UINT64 i = IBQ_size - 1; i >= 0; i--) {
        int index = IBQ_tail - 1 - i;
        if (index < 0) {
            index = index + IBQ_SIZE;
        }
        // Look for loads and stores
        // First case: uncommitted store. Its committed after a number of cycles of IBQ time
        if (INS_IsMemoryWrite(IBQ[index].ins) && !IBQ[index].committed && i >= STORE_RESOLVE_CYCLES) {
            // Should be committed
            IBQ[index].committed = true;
            // Update the MDST as well
            for (UINT64 j = 0; j < MDST_SIZE; j++) {
                if (MDST[j].valid && MDST[j].stpc == INS_Address(IBQ[index].ins) && MDST[j].stid == (UINT64) index)
                    // Mark the entry as complete
                    MDST[j].fe = true;
            }
            // Key observatoin: Only one store should ever commit in this loop (since its executed every cycle)
            // Makes determining mispredictions really easy
            committed_st = true;
            committed_st_index = index;
        }
        // Second case: its a committed load. Need to check for a misprediction
        else if (INS_IsMemoryRead(IBQ[index].ins) && IBQ[index].committed && committed_st) {
            // Check for mispredictions
            if (IBQ[index].ea == IBQ[committed_st_index].ea) {
                // Misprediction detected
                mispredictions++;
                // Look for an MDPT entry to update
                UINT64 j = 0;
                for (; j < MDPT_SIZE; j++)
                    if (MDPT[j].valid && MDPT[j].ldpc == INS_Address(IBQ[index].ins) && MDPT[j].stpc == INS_Address(IBQ[committed_st_index].ins))
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
                    mdpt.ldpc = INS_Address(IBQ[index].ins);
                    mdpt.stpc = INS_Address(IBQ[committed_st_index].ins);
                    mdpt.dist = diff;
                    mdpt.pred = 1;
                    mdpt.last_access = cycles;
                    UINT64 mdpt_index = allocateNewMDPTEntry();
                    MDPT[mdpt_index] = mdpt;
                }
            }
        }
        else if (INS_IsMemoryRead(IBQ[index].ins) && !IBQ[index].committed) {
            // Check MDST to see if the dependency was resolved
            for (UINT64 j = 0; j < MDST_SIZE; j++) {
                if (MDST[j].valid && MDST[j].ldpc == INS_Address(IBQ[index].ins) && MDST[j].ldid == (UINT64) index && MDST[j].fe) {
                    // Dependency was resolved.
                    // Check to see if it was a true dependency and update MDPT
                    // Find MDPT entry
                    UINT64 k = 0;
                    for (; k < MDPT_SIZE; k++)
                        if (MDPT[k].valid && MDPT[k].ldpc == INS_Address(IBQ[index].ins) && MDPT[k].stpc == MDST[j].stpc)
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

    // Create a new IBQ entry
    IBQ_entry ibq;
    ibq.ins = ins;
    ibq.ea = ea;
    // If the instruction is a load or a store, it won't be committed yet
    ibq.committed = !(INS_IsMemoryWrite(ins) || INS_IsMemoryRead(ins));
    
    // If the instruction was a load, we have to search for potential store dependencies in MDPT
    if (INS_IsMemoryRead(ins)) {
        ld_ins_count++;
        // Walk throught the MDPT, looking for historic store conflicts
        UINT64 i = 0;
        for (; i < MDPT_SIZE; i++)
            if (MDPT[i].valid && MDPT[i].ldpc == INS_Address(ins))
                break;
        if (i < MDPT_SIZE) {
            // Found an MDPT entry. Predict whether there is a dependency
            // We mark the instruction as committed if we predict no dependency
            ibq.committed = (MDPT[i].pred < 2);
        }
        else {
            // No MDPT entry found. We always predict no dependency here
            ibq.committed = true;
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
    if (INS_IsMemoryWrite(ins)) {
        st_ins_count++;
        // Walk through the MDPT, looking for previous load conflicts
        for (UINT64 i = 0; i < MDPT_SIZE; i++) {
            if (MDPT[i].valid && MDPT[i].stpc == INS_Address(ins) && MDPT[i].pred >= 2) {
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
    if (IBQ_tail >= IBQ_size)
        IBQ_tail = 0;
    if (IBQ_count < IBQ_size-1)
        IBQ_count++;
}
    
// Pin calls this function every time a new instruction is encountered
VOID Instruction(INS ins, VOID *v)
{
    // Insert a call to docount before every instruction, no arguments are passed
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)docount, ins, IARG_MEMORYOP_EA, ins, IARG_END);
}

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "loadStore.out", "specify output file name");

// This function is called when the application exits
VOID Fini(INT32 code, VOID *v)
{
    // Write to a file since cout and cerr maybe closed by the application
    OutFile.setf(ios::showbase);
    OutFile << "Count " << cycles << " input  " << input << endl;
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
