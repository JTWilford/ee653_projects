#include "svm_data.h"
#include <cstdlib>

int sv_n = CLASS_NUM * VEC_PER_CLASS;
int class_num = CLASS_NUM;
int input_size = INPUT_SIZE;

double fRand(double fMin, double fMax) {
    double f = (double)rand() / RAND_MAX;
    return fMin + f * (fMax - fMin);
}

svm_model* model_fill_random() {
	svm_model* m = (svm_model*) malloc(sizeof(svm_model));
	m->param.svm_type = C_SVC;
	m->param.kernel_type = LINEAR;
	m->nr_class = class_num;
	m->l = sv_n;

	// randomly initiate support vectors
	m->SV = (svm_node**) malloc(sizeof(svm_node*) * sv_n);
	if (m->SV == NULL)
		return NULL;
	for (int i = 0; i < sv_n; i++) {
		m->SV[i] = (svm_node*) malloc(sizeof(svm_node) * input_size);
		if (m->SV[i] == NULL)
			return NULL;

		for (int j = 0; j < input_size; j++) {
			m->SV[i][j].index = 0;
			m->SV[i][j].value = fRand(-200, 200);
		}
		m->SV[i][input_size-1].index = -1;
	}

	// randomly initiate sv_coef
	m->sv_coef = (double**) malloc(sizeof(double*) * (class_num-1));
	if (m->sv_coef == NULL)
		return NULL;

	for (int i = 0; i < class_num-1; i++) {
		m->sv_coef[i] = (double*) malloc(sizeof(double) * sv_n);
		if (m->sv_coef[i] == NULL)
			return NULL;

		for (int j = 0; j < sv_n; j++)
			m->sv_coef[i][j] = fRand(-400, 400);
	}

	int rho_size = ((class_num-1) * class_num / 2);
	m->rho = (double*) malloc(sizeof(double) * rho_size);
	if (m->rho == NULL)
		return NULL;

	for (int i = 0; i < rho_size; i++)
		m->rho[i] = fRand(-500, 500);

    m->label = (int*) malloc(sizeof(int) * class_num);
    if (m->label == NULL)
    	return NULL;

    for (int i = 0; i < class_num; i++)
    	m->label[i] = i;

    m->nSV = (int*) malloc(sizeof(int) * sv_n);
    if (m->nSV == NULL)
    	return NULL;

    for (int i = 0; i < class_num; i++)
    	m->nSV[i] = sv_n / class_num;

    return m;
}

svm_node* input_fill_random() {
	svm_node* n = (svm_node*) malloc(sizeof(svm_node) * input_size);
	if (n == NULL)
		return NULL;
	for (int i = 0; i < input_size; i++) {
		n[i].index = 0;
		n[i].value = fRand(-600, 600);
	}
	return n;
}

void destroy_model(svm_model* m) {
	if (m == NULL)
		return;

	// freeing SVs
	if (m->SV)
		for (int i = 0; i < m->l; i++)
			if (m->SV[i])
				free(m->SV[i]);

	if (m->sv_coef)
		for (int i = 0; i < m->nr_class-1; i++)
			if (m->sv_coef[i])
				free(m->sv_coef[i]);

	if (m->rho)
		free(m->rho);

	if (m->label)
		free(m->label);

	if (m->nSV)
		free(m->nSV);
	free(m);
}

void destroy_input(svm_node* n) {
	if (n)
		free(n);
}