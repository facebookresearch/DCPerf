include "common/fb303/if/fb303.thrift"

namespace cpp2 facebook.cea.chips.adsim
namespace py3 facebook.cea.chips.adsim

struct AdSimRequest {
  1: string request;
}

struct AdSimResponse {
  1: string response;
}

service AdSim extends fb303.FacebookService {
  AdSimResponse sim(1: AdSimRequest req);
}
