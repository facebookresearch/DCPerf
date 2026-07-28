/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// mock_cuda.h — Python-callable C functions for CUDA driver mocking.

#pragma once
#include <Python.h>

PyObject* patch_mock_cuda(PyObject*, PyObject*);
PyObject* enable_mock_cuda(PyObject*, PyObject*);
PyObject* disable_mock_cuda(PyObject*, PyObject*);
