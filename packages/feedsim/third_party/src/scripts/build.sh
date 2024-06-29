#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

function clean_up {
    # Clean up before exiting script
    kill %socat
    unset HTTPS_PROXY
    unset HTTP_PROXY
    unset no_proxy
}
trap clean_up EXIT

export no_proxy=".fbcdn.net,.facebook.com,.thefacebook.com,.tfbnw.net,.fb.com,.fburl.com,.facebook.net,.sb.fbsbx.com,localhost"

# Set up the fwdproxy for devservers
socat tcp-listen:9876,fork \
    openssl-connect:fwdproxy:8082,cert=/var/facebook/x509_identities/server.pem,cafile=/var/facebook/rootcanal/ca.pem,commonname=svc:fwdproxy &

set -e
HTTPS_PROXY='http://localhost:9876' HTTP_PROXY='http://localhost:9876' cmake ../ -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja
