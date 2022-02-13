#ifndef _SVM_DATA_H_
#define _SVM_DATA_H_

#include "svm.h"

svm_model* model_fill_random();
svm_node* input_fill_random();
void destroy_model(svm_model* m);
void destroy_input(svm_node* n);

#endif