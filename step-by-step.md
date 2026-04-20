# Multi-Container Runtime: Step-by-Step Execution Guide

This guide starts from your current state:
- Ubuntu 24.04 VM is ready
- preflight checks are done
- rootfs copies already exist (rootfs-alpha, rootfs-beta)

Follow commands exactly in order.

Privilege note:
- If supervisor is started with `sudo`, run all `engine` CLI commands with `sudo` too.
- Alternative: open a root shell once using `sudo -s` and run `./engine ...` commands there.
- Running `./engine ps` as a normal user while supervisor socket is root-owned can show: `Failed to connect to supervisor (/tmp/mini_runtime.sock): Permission denied`.

## 0) Session Setup (Do Once Per Demo Day)

Open 3 terminals in your VM:
- Terminal A: supervisor
- Terminal B: CLI commands
- Terminal C: monitor and measurement commands

In Terminal B:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
make clean
make
make ci
```

Copy latest workloads into both rootfs copies:
 hello 
 
```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/
cp ./io_pulse ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/
cp ./memory_hog ./rootfs-alpha/
cp ./memory_hog ./rootfs-beta/
```

(Optional) If you want a fully fresh state before demos:

```bash
rm -rf ./rootfs-alpha ./rootfs-beta
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/
cp ./io_pulse ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/
cp ./memory_hog ./rootfs-alpha/
cp ./memory_hog ./rootfs-beta/
```

Load kernel monitor:

```bash
sudo insmod ./monitor.ko
ls -l /dev/container_monitor
```

Start supervisor in Terminal A:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine supervisor ./rootfs-base
```

Do not close Terminal A until Task 6.

---

## Task 1) Multi-Container Runtime with Parent Supervisor

Run these in Terminal B:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine start alpha ./rootfs-alpha "/bin/sleep 120"
sudo ./engine start beta ./rootfs-beta "/bin/sleep 120"
sudo ./engine ps
```

Verify container namespace behavior (inside one container command):

```bash
cp -a ./rootfs-base ./rootfs-nscheck
sudo ./engine run nscheck ./rootfs-nscheck 'echo HOSTNAME=$(hostname); ps'
sudo ./engine logs nscheck
rm -rf ./rootfs-nscheck
```

What to verify:
- supervisor keeps running in Terminal A
- both alpha and beta appear in ps output with running state
- nscheck logs show HOSTNAME and ps output (means /proc is mounted)

Screenshot checkpoint:
- SS-1 (Multi-container supervision): show two live containers under one supervisor using sudo ./engine ps

---

## Task 2) Supervisor CLI and Signal Handling

Run in Terminal B:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine ps
# Use the IDs shown by ps (for example alpha2/beta2 if that is what you started)
sudo ./engine logs alpha2
sudo ./engine stop alpha2
sudo ./engine stop beta2
sudo ./engine ps
```

Demonstrate run behavior (foreground wait):

```bash
# rootfs-alpha must be free (not used by any running container)
sudo ./engine run run1 ./rootfs-alpha "/bin/sleep 20"
```

Signal forwarding check:

```bash
# rootfs-beta must be free (not used by any running container)
sudo ./engine run run2 ./rootfs-beta "/bin/sleep 120"
```

While run2 is waiting, press Ctrl+C once in Terminal B.
Then check:

```bash
sudo ./engine ps
```

If the `run` command appears stuck for too long:
- open another terminal
- run `sudo ./engine ps` to confirm state transitions
- run `sudo ./engine stop run2` once
- if needed, stop only the client process (`pkill -f 'engine run run2'`) and continue from a fresh terminal

What to verify:
- CLI commands get response from supervisor
- stop changes state in metadata
- run blocks and returns final status
- Ctrl+C during run forwards stop intent

Screenshot checkpoint:
- SS-4 (CLI and IPC): show a CLI command and supervisor response

---

## Task 3) Bounded-Buffer Logging and IPC Design

Before Task 3 retry after code edits:
- rebuild (`make`)
- stop old supervisor and start a fresh one so it uses the new binary
- clear old log files if needed (`rm -f ./logs/*.log`)

Generate both stdout and stderr from one container:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine run logdemo ./rootfs-alpha "echo out-line; echo err-line 1>&2; /cpu_hog 8"
```

Inspect logs:

```bash
sudo ./engine logs logdemo
ls -l ./logs
```

Stress logging with two concurrent producers:

```bash
sudo ./engine start loga ./rootfs-alpha "/cpu_hog 12"
sudo ./engine start logb ./rootfs-beta "echo begin; /io_pulse 30 80; echo end"
sudo ./engine ps
```

Wait a bit and inspect:

```bash
sudo ./engine logs loga
sudo ./engine logs logb
```

What to verify:
- output is persisted in per-container log files
- log file has both stdout and stderr entries
- concurrent containers generate independent logs

Screenshot checkpoints:
- SS-2 (Metadata tracking): capture a clean sudo ./engine ps output with useful metadata
- SS-3 (Bounded-buffer logging): show sudo ./engine logs <id> plus logs directory evidence

---

## Task 4) Kernel Memory Monitoring (Soft + Hard Limits)

Use Terminal C for dmesg tracking:

```bash
# non-sudo dmesg may show "Operation not permitted" on Ubuntu
sudo dmesg -C
sudo dmesg -w | grep --line-buffered container_monitor
```

In Terminal B, verify workload binary exists in both rootfs copies:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
ls -l ./rootfs-alpha/memory_hog ./rootfs-beta/memory_hog
```

In Terminal B, soft-limit warning test:


```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine start softwarn ./rootfs-alpha "/memory_hog 8 200" --soft-mib 32 --hard-mib 512
```

After warning appears in Terminal C, stop it:

```bash
sudo ./engine stop softwarn
sudo ./engine ps
```

Hard-limit kill test:

```bash
sudo ./engine start hardkill ./rootfs-beta "/memory_hog 8 200" --soft-mib 16 --hard-mib 40
sleep 3
sudo ./engine ps
sudo dmesg | grep container_monitor | tail -n 50
```

What to verify:
- soft limit warning appears once in dmesg
- hard limit triggers SIGKILL via kernel module
- ps state shows killed for hard-limit case and stopped for manual stop

Screenshot checkpoints:
- SS-5 (Soft-limit warning): dmesg line with SOFT LIMIT
- SS-6 (Hard-limit enforcement): dmesg HARD LIMIT + sudo ./engine ps showing killed

---

## Task 5) Scheduler Experiments and Analysis

Use two experiment sets and record raw numbers.

### Experiment A: CPU vs CPU, different nice values

Start both at same time:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine start cpu_fast ./rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start cpu_slow ./rootfs-beta "/cpu_hog 20" --nice 10
```

Wait for both to exit:

```bash
watch -n 1 'sudo ./engine ps'
```

After completion, collect final lines:

```bash
sudo ./engine logs cpu_fast | tail -n 1
sudo ./engine logs cpu_slow | tail -n 1
```

Record accumulator values from both lines in your README results table.

### Experiment B: CPU-bound vs I/O-bound

```bash
sudo ./engine start cpu_mix ./rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start io_mix ./rootfs-beta "/io_pulse 60 100" --nice 0
watch -n 1 'sudo ./engine ps'
sudo ./engine logs cpu_mix | tail -n 20
sudo ./engine logs io_mix | tail -n 20
```

Record:
- completion timing observations
- I/O pulse responsiveness while CPU hog is active
- any fairness/throughput differences

Screenshot checkpoint:
- SS-7 (Scheduling experiment): include commands + measurable output differences

---

## Task 6) Resource Cleanup Verification

Stop any running containers:

```bash
cd ~/OS-Jackfruit-palash-oj/boilerplate
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine stop loga
sudo ./engine stop logb
sudo ./engine stop cpu_fast
sudo ./engine stop cpu_slow
sudo ./engine stop cpu_mix
sudo ./engine stop io_mix
sudo ./engine ps
```

Stop supervisor cleanly:
- go to Terminal A and press Ctrl+C

Verify no zombies/stale runtime state:

```bash
ps -eo pid,ppid,stat,cmd | grep -E 'engine|cpu_hog|io_pulse|memory_hog' | grep -v grep
ps -eo stat,cmd | grep ' Z ' || true
ls -l /tmp/mini_runtime.sock || echo "control socket removed"
```

Unload module and inspect final logs:

```bash
dmesg | tail -n 100
sudo rmmod monitor
lsmod | grep monitor || true
```

Screenshot checkpoint:
- SS-8 (Clean teardown): show no zombies plus supervisor/module cleanup evidence

---

## Final Submission Packaging (Must Match project-guide.md)

From repository root:

```bash
cd ~/OS-Jackfruit-palash-oj
```

Ensure required source files exist:

```bash
ls boilerplate/engine.c
ls boilerplate/monitor.c
ls boilerplate/monitor_ioctl.h
ls boilerplate/Makefile
ls boilerplate/cpu_hog.c
ls boilerplate/io_pulse.c
ls boilerplate/memory_hog.c
```

Run CI-safe compile check exactly:

```bash
make -C boilerplate ci
./boilerplate/engine || true
```

Prepare README for submission (replace starter README with your report):

```bash
cp ./readme_report.md ./README.md
```

Then edit README.md and fill all required sections:
- team information
- reproducible build/load/run instructions
- all 8 annotated screenshots
- engineering analysis (5 required areas)
- design decisions/tradeoffs
- scheduler experiment raw data and interpretation

Check git status before commit:

```bash
git status
```

Important:
- do not commit rootfs-base or rootfs-* directories
- do not commit build artifacts
- commit source + README + docs/screenshots paths only

---

## Quick Screenshot Map

- SS-1: multi-container supervision (Task 1)
- SS-2: metadata tracking output of sudo ./engine ps (Task 3 section)
- SS-3: bounded-buffer logging evidence (Task 3)
- SS-4: CLI + IPC command/response (Task 2)
- SS-5: soft-limit warning in dmesg (Task 4)
- SS-6: hard-limit kill evidence + metadata state (Task 4)
- SS-7: scheduler experiment measurable comparison (Task 5)
- SS-8: clean teardown and no zombies (Task 6)
