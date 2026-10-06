# Quick setup: a clean campaign on Ubuntu

Step-by-step checklist for running the full campaign (`run_everything.sh`) on the
measurement machine from scratch. For what each script does, see `README.md`.

## 1. Boot into text mode

Running without the desktop removes its background activity and gives a lower, steadier
idle power for RAPL. Make text mode the default and reboot:

```bash
sudo systemctl set-default multi-user.target
sudo reboot
```

The machine boots into a text console; log in there. Step 7 brings the desktop back.
Do not stop the desktop in place with `systemctl isolate multi-user.target`: with some
graphics drivers the console never comes back and only a hard reset recovers it.

After booting, record the kernel for the paper:

```bash
uname -r
```

Automatic updates can install a new kernel that only takes effect at the next boot, so
it may differ from the last campaign's. A proprietary graphics driver (e.g. NVIDIA) may
also be missing for a new kernel: the desktop then falls back to a low-resolution mode.
Neither matters in text mode, where the graphics card is not used, but the kernel
version goes into the paper (`run.sh` also saves it in `global_results/system_info.txt`).

## 2. Update the repository, on `main`

The campaign runs from `main`. Check the branch first:

```bash
cd <path>/transport-performance-lab
git fetch origin
git status -sb              # first line: ## main...origin/main
```

If it shows another branch, make sure it holds nothing that `main` lacks before
switching. Both commands must print nothing:

```bash
git status --short                      # uncommitted changes
git log --oneline origin/main..HEAD     # local commits not in main
git switch main                         # only if both printed nothing
```

Then update:

```bash
git pull
git status -sb              # ## main...origin/main
git status --short          # no modified tracked files
```

## 3. Delete every previous result

The scripts resume: a scenario with a "done" checkpoint in `<arm>/results/` is skipped
and keeps its old data, and `run.sh` restores `*__preexisting_backup_*` copies. To start
from zero, remove all of it. **This deletes every previous result, with no copy.**

```bash
for d in asio taps-asio async-berkeley bsd-sockets capy-corosio; do sudo rm -rf "$d/results"; done
sudo rm -rf global_results results_netem campaign_logs campaign_status.txt idle_baseline.json
```

Check that it is clean. Both commands must print **nothing**:

```bash
find . -path ./bsd-sockets-v -prune -o \( -name scenario_done.json -o -name '*__preexisting_backup_*' -o -name '*__netem_rtt_*' \) -print
ls -d global_results results_netem campaign_logs campaign_status.txt idle_baseline.json 2>/dev/null
```

Build directories, the payload and the TLS certificates need no cleaning: the build
stage recreates the build directories and regenerates whatever is missing or does not
match its hash. `idle_baseline.json` is measured again when the campaign starts.

## 4. Stop automatic updates

Not done by any script in this repository. Over a multi-day campaign, apt and snap can
wake up in the middle of a measurement and add CPU and disk load.

```bash
sudo systemctl stop apt-daily.timer apt-daily-upgrade.timer unattended-upgrades
sudo snap refresh --hold
```

## 5. Tune hardware and OS

Sets the governor to `performance` and turns off Turbo Boost, SMT, ASLR, the NMI
watchdog, transparent huge pages and swap. It does not survive a reboot, so run it right
before launching.

```bash
sudo ./tune_machine.sh status      # if it reports a state file from an earlier run,
                                   # first: sudo ./tune_machine.sh restore
sudo ./tune_machine.sh apply       # no FAIL line may appear
```

The state file (`.tune_machine_state`) survives a reboot although the settings do not,
and `apply` refuses to run while it exists, so that it never overwrites the machine's
original values. After a reboot, `restore` just writes those original values again and
removes the file.

## 6. Launch

```bash
sudo ./run_everything.sh
```

Keep this console open and do not press Ctrl-C. The campaign measures idle power at the
start, with the machine already in text mode and tuned, so idle and load measurements
share the same state.

To follow it, switch to another console (**Ctrl+Alt+F4**):

- `cat campaign_status.txt`: one line with the current stage, or why it stopped.
- `campaign_logs/<date>/campaign.log`: the full log.
- ntfy notifications (topic `jmcruz-taps`): when the baseline campaign ends, after each
  of the 12 D7 points, at the end, and on any failure. The network stays up in text mode.

## 7. When it finishes, undo everything

```bash
sudo ./tune_machine.sh restore
sudo snap refresh --unhold
sudo systemctl start apt-daily.timer apt-daily-upgrade.timer unattended-upgrades
sudo systemctl set-default graphical.target
sudo reboot                                  # boots with the desktop again
```
