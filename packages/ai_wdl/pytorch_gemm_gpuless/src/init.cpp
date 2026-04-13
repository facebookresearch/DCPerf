/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// init.cpp — Python module definition for _mock_cuda_C extension.

#include <Python.h>
#include "mock_cuda.h"

static PyMethodDef _mock_cuda_C_methods[] = {
    {"patch_mock_cuda",
     patch_mock_cuda,
     METH_NOARGS,
     "Patch libcuda.so.1 function table for mocking."},
    {"enable_mock_cuda",
     enable_mock_cuda,
     METH_NOARGS,
     "Enable CUDA mocking (thread-local)."},
    {"disable_mock_cuda",
     disable_mock_cuda,
     METH_NOARGS,
     "Disable CUDA mocking (thread-local)."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef _mock_cuda_C_module = {
    PyModuleDef_HEAD_INIT,
    "_mock_cuda_C",
    "CUDA driver function table patching for GPU-less benchmarking.",
    -1,
    _mock_cuda_C_methods};

PyMODINIT_FUNC PyInit__mock_cuda_C(void) {
  return PyModule_Create(&_mock_cuda_C_module);
}
