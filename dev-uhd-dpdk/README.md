## Procedure to build FlexSDR dev with DPDK

## Pre-requisties
- dpdk
- uhd 4.8.0.0

### Installations
- cd build
- cmake ..
- make

### Command execution
```
[Terminal 1]: sudo ./build/test_dpdk_primary_only
[Terminal 2]: sudo ./build/test_flexsdr_factory --role ue
```

### Test Logs
```
Primary
sudo ./build/test_dpdk_primary_only 
--- ring stats --- 
UE_in ring=ue_inbound_ring used=512 free=0 cap=512 
UE_tx0 ring=ue_tx_ch1 used=0 free=512 cap=512
gNB_in ring=gnb_inbound_ring used=0 free=512 cap=512 
gNB_tx0 ring=gnb_tx_ch1 used=0 free=512 cap=512

Secondary
[UHD] device OK.
[RX] receiving 4ch… Ctrl+C to stop
got=2048 | ch0=14142.2 ch1=14142.2 ch2=14142.2 ch3=14142.2 | ts=yes | EOB
got=2048 | ch0=14142.2 ch1=14142.2 ch2=14142.2 ch3=14142.2 | ts=yes | EOB
got=2048 | ch0=14142.2 ch1=14142.2 ch2=14142.2 ch3=14142.2 | ts=yes | EOB
got=2048 | ch0=14142.2 ch1=14142.2 ch2=14142.2 ch3=14142.2 | ts=yes | EOB
got=2048 | ch0=14142.2 ch1=14142.2 ch2=14142.2 ch3=14142.2 | ts=yes | EOB
```




