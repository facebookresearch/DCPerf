namespace cpp2 facebook.cea.chips.adsim

cpp_include "list"
cpp_include "folly/container/F14Map.h"

include "thrift/annotation/cpp.thrift"

struct SerializableInfo {
  1: bool var_bool;
  2: i32 var_i32;
  3: double var_double;
}

@cpp.Type{template = "std::list"}
typedef list<i32> id_list_t
@cpp.Type{template = "folly::F14VectorMap"}
typedef map<i32, id_list_t> id_map_t
@cpp.Type{template = "std::list"}
typedef list<SerializableInfo> info_list_t

struct SerializableUnit {
  1: id_map_t map_of_list;
  2: info_list_t list_of_info;
  3: id_list_t list_of_i32;
}

@cpp.Type{template = "std::list"}
typedef list<SerializableUnit> unit_list_t

struct SerializableReq {
  1: i64 var_i64;
  2: list<SerializableUnit> list_of_unit;
}
