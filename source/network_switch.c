#include "network.h"
#include <errno.h>
#include <switch.h>

int net_read_config(NetConfig *config) {
  if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
    errno = ENETDOWN;
    return -1;
  }
  const Result result = nifmGetCurrentIpConfigInfo(&config->address, &config->netmask,
      &config->gateway, &config->dns[0], &config->dns[1]);
  nifmExit();
  if (R_FAILED(result) || !config->address) {
    errno = ENETDOWN;
    return -1;
  }
  return 0;
}
