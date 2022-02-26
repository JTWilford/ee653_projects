# EE 653 Lab 1 - MDPT and MDST Simulation with PIN
Kyle Tseng <kyletsen@usc.edu>
Justin Wilford <jwilford@usc.edu>

## Running the Simulation
Execute the following command while in this directory:

        make

This command will compile the loadStore PIN application, and then run the
benchmark for Store Resolution Windows of 10, 20, and 30 cycles.

The results of each simulation will be saved in "loadStore_%i%.txt", where %i%
is the Store Resolution Window.

## Metrics Captured
Our output file includes the following metrics:
- Total Instructions
    - The number of instructions executed during the simulation
- Total Loads
    - The number of Loads executed during the simulation
- Total Stores
    - The number of stores executed during the simulation
- Total MDPT Predictions
    - The number of predictions made by the MDPT
        Predictions are only counted when an MDPT entry exists between
        the conflicting Loads and Stores (not for initial conflicts, where
        a load is always predicted independent)
- Total MDPT Mispredictions: 4168
    - The number of times an MDPT prediction was incorrect
- Misprediction Rate: 0.0943712
    - MDPT Mispredictions / MDPT Predictions
- Total Load Speculations: 278732
    - The number of times a load was speculatively committed
        This includes when a Load without a dependence history enters while
        there is a store in the IBQ within the Store Resolution Window
- Total Load Mis-speculations: 38244
    - The number of times a speculation was incorrect
- Mis-speculation Rate: 0.137207
    - Mis-speculations / Speculations
- Total False Dependencies: 74
    - The number of False Dependencies caught
        A false dependencie occurs when a Load was predicted dependent, but
        when the Store address is resolved no dependence was found
- Mis-speculations due to False Dependencies: 0.00193494
    - False Dependencies / Mis-speculations
- Avg. Time in LD/ST Buffer (Loads): 2.81836
    - The average time a Load spends waiting for its address. In this
        simulation, the following assumptions were made: 
        - a load with no dependence is assumed to stay in the LD/ST buffer
            for 1 cycle
        - When a speculation error occurs, the load returns to the LD/ST
            buffer for an additional 1 cycle
        - A load that is predicted dependent stays in the LD/ST buffer
            until its dependency is resolved


## Notes on Implementation
There is a single instance in the application where we walk a distance in the
IBQ equal to the Store Resolution Window. This is done to detect initial Load
Store Dependencies when a conflict occurs. We decided it was necessary to do
this walk because there is no CDB in the system to announce a store's resolved
address to loads in the pipeline. Every other case is handled solely through
the MDPT and MDST.