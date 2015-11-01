/*
 *  Copyright 2007-2015 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "omxExpectation.h"
#include "omxFitFunction.h"
#include "omxBLAS.h"
#include "omxRAMExpectation.h"

typedef struct {

	omxMatrix *cov, *means; // observed covariance and means
	omxMatrix *A, *S, *F, *M, *I;
	omxMatrix *X, *Y, *Z, *Ax;

	int numIters;
	double logDetObserved;
	double n;
	double *work;
	int lwork;

} omxRAMExpectation;

static void omxCalculateRAMCovarianceAndMeans(omxMatrix* A, omxMatrix* S, omxMatrix* F, 
    omxMatrix* M, omxMatrix* Cov, omxMatrix* Means, int numIters, omxMatrix* I, 
    omxMatrix* Z, omxMatrix* Y, omxMatrix* X, omxMatrix* Ax);
static omxMatrix* omxGetRAMExpectationComponent(omxExpectation* ox, const char* component);

static void omxCallRAMExpectation(omxExpectation* oo, const char *, const char *) {
    if(OMX_DEBUG) { mxLog("RAM Expectation calculating."); }
	omxRAMExpectation* oro = (omxRAMExpectation*)(oo->argStruct);
	
	omxRecompute(oro->A, NULL);
	omxRecompute(oro->S, NULL);
	omxRecompute(oro->F, NULL);
	if(oro->M != NULL)
	    omxRecompute(oro->M, NULL);
	    
	omxCalculateRAMCovarianceAndMeans(oro->A, oro->S, oro->F, oro->M, oro->cov, 
		oro->means, oro->numIters, oro->I, oro->Z, oro->Y, oro->X, oro->Ax);
}

static void omxDestroyRAMExpectation(omxExpectation* oo) {

	if(OMX_DEBUG) { mxLog("Destroying RAM Expectation."); }
	
	omxRAMExpectation* argStruct = (omxRAMExpectation*)(oo->argStruct);

	omxFreeMatrix(argStruct->cov);

	if(argStruct->means != NULL) {
		omxFreeMatrix(argStruct->means);
	}

	omxFreeMatrix(argStruct->I);
	omxFreeMatrix(argStruct->X);
	omxFreeMatrix(argStruct->Y);
	omxFreeMatrix(argStruct->Z);
	omxFreeMatrix(argStruct->Ax);

}

static void omxPopulateRAMAttributes(omxExpectation *oo, SEXP algebra) {
    if(OMX_DEBUG) { mxLog("Populating RAM Attributes."); }

	omxRAMExpectation* oro = (omxRAMExpectation*) (oo->argStruct);
	omxMatrix* A = oro->A;
	omxMatrix* S = oro->S;
	//omxMatrix* X = oro->X;
	omxMatrix* Ax= oro->Ax;
	omxMatrix* Z = oro->Z;
	omxMatrix* I = oro->I;
    int numIters = oro->numIters;
    double oned = 1.0, zerod = 0.0;
    
    omxRecompute(A, NULL);
    omxRecompute(S, NULL);
	
    omxShallowInverse(NULL, numIters, A, Z, Ax, I ); // Z = (I-A)^-1
	
	omxDGEMM(FALSE, FALSE, oned, Z, S, zerod, Ax);

	omxDGEMM(FALSE, TRUE, oned, Ax, Z, zerod, Ax);
	// Ax = ZSZ' = Covariance matrix including latent variables
	
	{
	SEXP expCovExt;
	ScopedProtect p1(expCovExt, Rf_allocMatrix(REALSXP, Ax->rows, Ax->cols));
	for(int row = 0; row < Ax->rows; row++)
		for(int col = 0; col < Ax->cols; col++)
			REAL(expCovExt)[col * Ax->rows + row] =
				omxMatrixElement(Ax, row, col);
	Rf_setAttrib(algebra, Rf_install("UnfilteredExpCov"), expCovExt);
	}
	Rf_setAttrib(algebra, Rf_install("numStats"), Rf_ScalarReal(omxDataDF(oo->data)));
}

/*
 * omxCalculateRAMCovarianceAndMeans
 * 			Just like it says on the tin.  Calculates the mean and covariance matrices
 * for a RAM model.  M is the number of total variables, latent and manifest. N is
 * the number of manifest variables.
 *
 * params:
 * omxMatrix *A, *S, *F 	: matrices as specified in the RAM model.  MxM, MxM, and NxM
 * omxMatrix *M				: vector containing model implied means. 1xM
 * omxMatrix *Cov			: On output: model-implied manifest covariance.  NxN.
 * omxMatrix *Means			: On output: model-implied manifest means.  1xN.
 * int numIterations		: Precomputed number of iterations of taylor series expansion.
 * omxMatrix *I				: Identity matrix.  If left NULL, will be populated.  MxM.
 * omxMatrix *Z				: On output: Computed (I-A)^-1. MxM.
 * omxMatrix *Y, *X, *Ax	: Space for computation. NxM, NxM, MxM.  On exit, populated.
 */

static void omxCalculateRAMCovarianceAndMeans(omxMatrix* A, omxMatrix* S, omxMatrix* F, 
	omxMatrix* M, omxMatrix* Cov, omxMatrix* Means, int numIters, omxMatrix* I, 
	omxMatrix* Z, omxMatrix* Y, omxMatrix* X, omxMatrix* Ax) {
	
	if(OMX_DEBUG) { mxLog("Running RAM computation with numIters is %d\n.", numIters); }
		
	if(Ax == NULL || I == NULL || Z == NULL || Y == NULL || X == NULL) {
		Rf_error("Internal Error: RAM Metadata improperly populated.  Please report this to the OpenMx development team.");
	}
		
	if(Cov == NULL && Means == NULL) {
		return; // We're not populating anything, so why bother running the calculation?
	}
	
	omxShallowInverse(NULL, numIters, A, Z, Ax, I );
	
	/* Cov = FZSZ'F' */
	omxDGEMM(FALSE, FALSE, 1.0, F, Z, 0.0, Y);

	omxDGEMM(FALSE, FALSE, 1.0, Y, S, 0.0, X);

	omxDGEMM(FALSE, TRUE, 1.0, X, Y, 0.0, Cov);
	 // Cov = FZSZ'F' (Because (FZ)' = Z'F')
	
	if(OMX_DEBUG_ALGEBRA) {omxPrintMatrix(Cov, "....RAM: Model-implied Covariance Matrix:");}
	
	if(M != NULL && Means != NULL) {
		// F77_CALL(omxunsafedgemv)(Y->majority, &(Y->rows), &(Y->cols), &oned, Y->data, &(Y->leading), M->data, &onei, &zerod, Means->data, &onei);
		omxDGEMV(FALSE, 1.0, Y, M, 0.0, Means);
		if(OMX_DEBUG_ALGEBRA) {omxPrintMatrix(Means, "....RAM: Model-implied Means Vector:");}
	}
}

void omxInitRAMExpectation(omxExpectation* oo) {
	
	omxState* currentState = oo->currentState;	
	SEXP rObj = oo->rObj;

	if(OMX_DEBUG) { mxLog("Initializing RAM expectation."); }
	
	int l, k;

	SEXP slotValue;
	
	omxRAMExpectation *RAMexp = (omxRAMExpectation*) R_alloc(1, sizeof(omxRAMExpectation));
	
	/* Set Expectation Calls and Structures */
	oo->computeFun = omxCallRAMExpectation;
	oo->destructFun = omxDestroyRAMExpectation;
	oo->componentFun = omxGetRAMExpectationComponent;
	oo->populateAttrFun = omxPopulateRAMAttributes;
	oo->argStruct = (void*) RAMexp;
	
	/* Set up expectation structures */
	if(OMX_DEBUG) { mxLog("Initializing RAM expectation."); }

	if(OMX_DEBUG) { mxLog("Processing M."); }
	RAMexp->M = omxNewMatrixFromSlot(rObj, currentState, "M");

	if(OMX_DEBUG) { mxLog("Processing A."); }
	RAMexp->A = omxNewMatrixFromSlot(rObj, currentState, "A");

	if(OMX_DEBUG) { mxLog("Processing S."); }
	RAMexp->S = omxNewMatrixFromSlot(rObj, currentState, "S");

	if(OMX_DEBUG) { mxLog("Processing F."); }
	RAMexp->F = omxNewMatrixFromSlot(rObj, currentState, "F");

	/* Identity Matrix, Size Of A */
	if(OMX_DEBUG) { mxLog("Generating I."); }
	RAMexp->I = omxNewIdentityMatrix(RAMexp->A->rows, currentState);

	if(OMX_DEBUG) { mxLog("Processing expansion iteration depth."); }
	{ScopedProtect p1(slotValue, R_do_slot(rObj, Rf_install("depth")));
	RAMexp->numIters = INTEGER(slotValue)[0];
	if(OMX_DEBUG) { mxLog("Using %d iterations.", RAMexp->numIters); }
	}

	l = RAMexp->F->rows;
	k = RAMexp->A->cols;

	if (k != RAMexp->S->cols || k != RAMexp->S->rows || k != RAMexp->A->rows) {
		Rf_error("RAM matrices '%s' and '%s' must have the same dimensions",
			 RAMexp->S->name(), RAMexp->A->name());
	}

	if(OMX_DEBUG) { mxLog("Generating internals for computation."); }

	RAMexp->Z = 	omxInitMatrix(k, k, TRUE, currentState);
	RAMexp->Ax = 	omxInitMatrix(k, k, TRUE, currentState);
	RAMexp->Ax->rownames = RAMexp->S->rownames;
	RAMexp->Ax->colnames = RAMexp->S->colnames;
	RAMexp->Y = 	omxInitMatrix(l, k, TRUE, currentState);
	RAMexp->X = 	omxInitMatrix(l, k, TRUE, currentState);
	
	RAMexp->cov = 		omxInitMatrix(l, l, TRUE, currentState);

	if(RAMexp->M != NULL) {
		RAMexp->means = 	omxInitMatrix(1, l, TRUE, currentState);
	} else {
	    RAMexp->means  = 	NULL;
    }
}

static omxMatrix* omxGetRAMExpectationComponent(omxExpectation* ox, const char* component) {
	
	if(OMX_DEBUG) { mxLog("RAM expectation: %s requested--", component); }

	omxRAMExpectation* ore = (omxRAMExpectation*)(ox->argStruct);
	omxMatrix* retval = NULL;

	if(strEQ("cov", component)) {
		retval = ore->cov;
	} else if(strEQ("means", component)) {
		retval = ore->means;
	} else if(strEQ("pvec", component)) {
		// Once implemented, change compute function and return pvec
	}
	
	return retval;
}
