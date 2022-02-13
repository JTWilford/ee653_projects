#include <cstdlib>
#include <iostream>
#include <chrono>

#include "svm.h"
#include "svm_data.h"

using namespace std;

int main(int argc, char** argv) {
	if (argc < 2) {
		cout << "check input arguments" << endl;
		return EXIT_FAILURE;	
   }
   using namespace std::chrono;

   int it = stoi(argv[1]);
   // random iteration
   for (int i = 0; i < it; i++) {
      // cout << "filling input" << endl;
		svm_node* input = input_fill_random();
      // cout << "filling model" << endl;
		svm_model* model = model_fill_random();
      // cout << "computing" << endl;
      high_resolution_clock::time_point t1 = high_resolution_clock::now();
    	svm_predict(model, input);
      high_resolution_clock::time_point t2 = high_resolution_clock::now();
      duration<double> time_span = duration_cast<duration<double>>(t2 - t1);
      cout << CLASS_NUM << ", " << time_span.count() << endl;
      destroy_model(model);
      destroy_input(input);
    }
	return EXIT_SUCCESS;
}
