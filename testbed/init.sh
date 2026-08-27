#!/bin/sh
export PATH=/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mount -t tmpfs none /tmp

echo 20 > /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null

echo "===== KERNEL: $(cat /proc/sys/kernel/osrelease) ====="
echo "===== TESTBED START ====="

run_corruption() {
    fs="$1"; dir="$2"
    echo "--- corruption test (splice) on $fs ---"
    n_ok=0; n_bad=0
    for i in $(seq 1 10); do
        out=$(cd "$dir" && TMPDIR="$dir" splice_race 2>/dev/null | grep 'final size')
        sz=${out##*=}
        if [ "$sz" = "96012" ]; then n_ok=$((n_ok+1)); else n_bad=$((n_bad+1)); fi
    done
    echo "RESULT splice-corruption $fs: intact=$n_ok corrupt=$n_bad (expect size 96012)"

    echo "--- corruption test (sendfile) on $fs ---"
    s_ok=0; s_bad=0
    for i in $(seq 1 5); do
        out=$(cd "$dir" && TMPDIR="$dir" sendfile_race 2>/dev/null | grep 'final size')
        got=${out#*size=}; got=${got%% *}
        exp=${out#*expected }; exp=${exp%)*}
        if [ "$got" = "$exp" ]; then s_ok=$((s_ok+1)); else s_bad=$((s_bad+1)); fi
    done
    echo "RESULT sendfile-corruption $fs: intact=$s_ok corrupt=$s_bad"
}

run_deadlock() {
    fs="$1"; dir="$2"
    echo "--- liveness/deadlock test on $fs ---"
    ( cd "$dir" && TMPDIR="$dir" splice_deadlock > /tmp/dl.out 2>&1 ) &
    tp=$!
    hung=1
    for i in $(seq 1 30); do
        if ! kill -0 $tp 2>/dev/null; then hung=0; break; fi
        sleep 1
    done
    if [ $hung = 1 ]; then
        echo "RESULT deadlock $fs: HUNG (no completion after 30s)"
        echo "--- blocked tasks ---"
        ps -o pid,stat,comm 2>/dev/null | grep -E 'splice_dead|STAT' || true
        echo "--- breaking the deadlock ---"
        pkill -9 splice_deadlock 2>/dev/null
        sleep 3
    else
        echo "RESULT deadlock $fs: COMPLETED"
    fi
    cat /tmp/dl.out 2>/dev/null
}

run_corruption tmpfs /tmp
run_deadlock  tmpfs /tmp

if [ -b /dev/vda ]; then
    mkdir -p /mnt/ext4
    if mount -t ext4 /dev/vda /mnt/ext4 2>/dev/null; then
        run_corruption ext4 /mnt/ext4
        run_deadlock  ext4 /mnt/ext4
        sync; umount /mnt/ext4
    else
        echo "NOTE: could not mount ext4 disk"
    fi
fi

echo "--- hung task / lockdep splats in dmesg ---"
dmesg | grep -iE 'hung task|blocked for more than|possible.*deadlock|lockdep|INFO: task' | head -40
echo "===== TESTBED END ====="
poweroff -f
