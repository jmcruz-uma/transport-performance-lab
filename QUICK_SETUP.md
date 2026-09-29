# Quick setup: a clean campaign on Ubuntu

Step-by-step checklist for running the full campaign (`run_everything.sh`) on the
measurement machine from scratch. For what each script does, see `README.md`.

## 1. Switch to text mode

Do this from a text console, not from a desktop terminal: closing the desktop would
close that terminal with it. Press **Ctrl+Alt+F3**, log in, and run:

```bash
sudo systemctl isolate multi-user.target     # stops the desktop; not permanent
```

This removes the desktop's background activity and gives a lower, steadier idle power
for RAPL.

## 2. Update the repository

```bash
cd <path>/transport-performance-lab
git pull
git status -sb              # must say it is up to date with origin/main
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
sudo systemctl isolate graphical.target      # brings the desktop back (a reboot does too)
```
