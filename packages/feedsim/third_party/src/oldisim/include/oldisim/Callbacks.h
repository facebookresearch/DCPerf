// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OLDISIM_CALLBACKS_H
#define OLDISIM_CALLBACKS_H

#include <functional>
#include <memory>

namespace oldisim {

class ChildConnection;
class FanoutManager;
class NodeThread;
class ParentConnection;
class QueryContext;
class ResponseContext;
class TestDriver;

typedef std::function<void(NodeThread&, ParentConnection&)> AcceptCallback;

typedef std::function<void(NodeThread&)> LeafNodeThreadStartupCallback;
typedef std::function<void(NodeThread&, QueryContext&)> LeafNodeQueryCallback;

typedef std::function<void(NodeThread&, FanoutManager&)>
    ParentNodeThreadStartupCallback;
typedef std::function<void(NodeThread&, FanoutManager&, QueryContext&)>
    ParentNodeQueryCallback;

typedef std::function<void(NodeThread&, TestDriver&)>
    DriverNodeThreadStartupCallback;
typedef std::function<void(NodeThread&, ResponseContext&)>
    DriverNodeResponseCallback;
typedef std::function<void(NodeThread&, TestDriver&)>
    DriverNodeMakeRequestCallback;
}  // namespace oldisim

#endif  // OLDISIM_CALLBACKS_H

