# DPDK Infrastructure Test Cases

This directory contains standalone test programs for validating DPDK multi-process setup for FlexSDR.

## Test Programs

### 1. testcase_primary_dpdk_infra
**Purpose**: Primary UE process that initializes DPDK EAL and creates shared resources (memory pools, rings).

**Role**: Acts as the **primary-ue** role, setting up:
- DPDK EAL in primary mode
- Memory pools for UE TX/RX operations
- TX rings: `ue_tx_ch1`, `ue_tx_ch2`
- RX rings: `ue_inbound_ring`

**Usage**:
```bash
./testcase_primary_dpdk_infra <path/to/configurations-ue.yaml>
```

**Example**:
```bash
./testcase_primary_dpdk_infra ../../conf/configurations-ue.yaml
```

### 2. testcase_secondary_dpdk_infra
**Purpose**: Secondary UE process that attaches to shared resources created by the primary.

**Role**: Acts as the **ue** (secondary) role, performing:
- DPDK EAL initialization in secondary mode
- Lookup of shared rings created by primary process

**Usage**:
```bash
./testcase_secondary_dpdk_infra <path/to/configurations-ue.yaml>
```

**Example**:
```bash
./testcase_secondary_dpdk_infra ../../conf/configurations-ue.yaml
```

**⚠️ IMPORTANT**: The primary process MUST be running before starting the secondary process!

## Building

From the `build-infra` directory (or wherever you build):

```bash
cd dev-uhd-dpdk/build-infra
cmake ../cmake/dpdk-infra
make -j$(nproc)
```

This will generate:
- `testcase_primary_dpdk_infra` - Primary process executable
- `testcase_secondary_dpdk_infra` - Secondary process executable
- `test_dpdk_infra` - Original orchestrator test (if present)

## Running the Tests

### Method 1: Manual Two-Terminal Approach (Recommended)

**Terminal 1 - Start Primary**:
```bash
cd dev-uhd-dpdk/build-infra
./testcase_primary_dpdk_infra ../../conf/configurations-ue.yaml
```

Wait for the message:
```
[primary-ue] Ready for secondary processes to attach.
[primary-ue] Press Ctrl+C to shutdown gracefully...
```

**Terminal 2 - Start Secondary**:
```bash
cd dev-uhd-dpdk/build-infra
./testcase_secondary_dpdk_infra ../../conf/configurations-ue.yaml
```

You should see:
```
[ue] ✓ All resources found successfully!
[ue] Secondary process is ready!
```

**Shutdown**: Press Ctrl+C in either terminal to stop gracefully.

### Method 2: Background Process

```bash
# Start primary in background
./testcase_primary_dpdk_infra ../../conf/configurations-ue.yaml &
PRIMARY_PID=$!

# Wait for primary to initialize (3-5 seconds)
sleep 5

# Start secondary
./testcase_secondary_dpdk_infra ../../conf/configurations-ue.yaml

# Cleanup
kill $PRIMARY_PID
```

## Expected Output

### Primary Process Success Output
```
========================================
FlexSDR Primary-UE DPDK Infrastructure Test
PID: 12345
========================================

[primary-ue] Loading config from: ../../conf/configurations-ue.yaml
[primary-ue] Initializing DPDK EAL...
[eal] init with args: flexsdr-primary-ue --file-prefix flexsdr-app --huge-dir /dev/hugepages --socket-mem 512,512 --iova va --no-pci --proc-type=primary
[primary-ue] EAL initialized successfully (consumed 11 args)
[primary-ue] FlexSDRPrimary constructed
[primary-ue] Initializing resources (pools, rings)...
[pool] created: ue_inbound_pool (n=8192 elt=2048 cache=256)
[pool] created: ue_outbound_pool (n=8192 elt=2048 cache=256)
[ring] created TX: ue_tx_ch1 (size=512)
[ring] created TX: ue_tx_ch2 (size=512)
[ring] created RX: ue_inbound_ring (size=512)

[primary-ue] ✓ All resources initialized successfully!
[primary-ue] Ready for secondary processes to attach.
```

### Secondary Process Success Output
```
========================================
FlexSDR Secondary-UE DPDK Infrastructure Test
PID: 12346
========================================

[ue] Loading config from: ../../conf/configurations-ue.yaml
[ue] Waiting for primary process to initialize resources...
[ue] Initializing DPDK EAL in secondary mode...
[eal] init with args: flexsdr-ue --file-prefix flexsdr-app --huge-dir /dev/hugepages --socket-mem 512,512 --iova va --no-pci --proc-type=secondary
[ue] EAL initialized successfully (consumed 11 args)
[ue] FlexSDRSecondary constructed
[ue] Looking up shared rings from primary...
[ring] found TX: ue_tx_ch1 (size=512)
[ring] found TX: ue_tx_ch2 (size=512)
[ring] found RX: ue_inbound_ring (size=512)

[ue] ✓ All resources found successfully!
[ue] Secondary process is ready!
```

## Troubleshooting

### Error: "EAL initialization failed"
**Cause**: DPDK environment not properly set up.

**Solutions**:
1. Ensure hugepages are configured:
   ```bash
   echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   ```
2. Check hugepage mount:
   ```bash
   mount | grep huge
   # Should show: hugetlbfs on /dev/hugepages type hugetlbfs
   ```
3. Verify DPDK libraries are installed and in library path

### Error: "Resource lookup failed" (Secondary)
**Cause**: Primary process not running or hasn't created resources yet.

**Solutions**:
1. Ensure primary process is running
2. Wait longer before starting secondary (increase wait time)
3. Check that both processes use the same config file
4. Verify `file_prefix` in config matches between runs

### Error: "AddressSanitizer: SEGV"
**Cause**: DPDK EAL not initialized before calling DPDK APIs.

**Solution**: This should be fixed in the new testcases. The old `test_dpdk_infra.cpp` had this issue.

### Error: "Cannot create pools/rings"
**Cause**: Insufficient hugepage memory or previous run didn't clean up.

**Solutions**:
1. Kill any existing DPDK processes:
   ```bash
   pkill -f testcase_primary
   pkill -f testcase_secondary
   ```
2. Clean up DPDK runtime files:
   ```bash
   sudo rm -rf /dev/hugepages/flexsdr-app*
   sudo rm -rf /var/run/dpdk/flexsdr-app*
   ```
3. Increase hugepage allocation

## Configuration Requirements

The test programs require a properly configured YAML file. Key requirements:

```yaml
eal:
  file_prefix: "flexsdr-app"      # Must be consistent across runs
  huge_dir: "/dev/hugepages"       # Must exist and be mounted
  socket_mem: "512,512"            # Adjust based on available memory
  no_pci: true                     # Set true if not using physical NICs
  iova: "va"                       # Virtual addressing mode

defaults:
  role: ue                         # For secondary; use primary-ue for primary
  ring_size: 512
  # ... other settings

primary-ue:
  pools:
    - { name: "ue_inbound_pool",  n: 8192, elt_size: 2048, cache: 256 }
    - { name: "ue_outbound_pool", n: 8192, elt_size: 2048, cache: 256 }
  tx_stream:
    rings:
      - { name: "ue_tx_ch1", size: 512 }
      - { name: "ue_tx_ch2", size: 512 }
  rx_stream:
    rings:
      - { name: "ue_inbound_ring", size: 512 }
```

## Key Differences from Original test_dpdk_infra.cpp

1. **Separate Executables**: Primary and secondary are now separate programs (easier to debug)
2. **EAL Initialization**: Properly initializes DPDK EAL before using DPDK APIs (fixes crash)
3. **Better Synchronization**: Secondary waits for primary to be ready
4. **Clear Error Messages**: More informative error reporting
5. **Graceful Shutdown**: Signal handlers for clean termination
6. **Simplified Testing**: No fork/exec complexity; straightforward process model

## Notes

- Both test programs use AddressSanitizer by default (can disable with `-DENABLE_ASAN=OFF`)
- Primary process must be kept running while secondary is active
- Each process logs with clear prefixes: `[primary-ue]` and `[ue]`
- Processes idle after initialization to allow inspection and testing
- Use Ctrl+C for graceful shutdown in both processes
