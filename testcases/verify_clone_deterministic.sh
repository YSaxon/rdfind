# verify_clone_deterministic.sh
#!/bin/sh
# Ensures that cloning is deterministic like other operations

set -e
. "$(dirname "$0")/common_funcs.sh"

# Only run on macOS/APFS
if [ "$(uname)" != "Darwin" ]; then
    echo "$me: not running on macOS, skipping APFS clone deterministic tests"
    exit 0
fi

if ! $hasdisorderfs ; then
    echo "$me: please install disorderfs to execute this test properly!"
    echo "$me: falsely exiting with success now"
    exit 0
fi

unmount_disordered() {
    if [ -d $DISORDERED_MNT ]; then
        fusermount -z -u $DISORDERED_MNT
    fi
}

DISORDERED_FLAGS_RANDOM="--shuffle-dirents=yes --sort-dirents=no --reverse-dirents=no"
DISORDERED_FLAGS_ASC="--shuffle-dirents=no --sort-dirents=yes --reverse-dirents=no"
DISORDERED_FLAGS_DESC="--shuffle-dirents=no --sort-dirents=yes --reverse-dirents=yes"
DISORDERED_FLAGS=$DISORDERED_FLAGS_RANDOM

mount_disordered() {
    mkdir -p $DISORDERED_MNT $DISORDERED_ROOT
    disorderfs $DISORDERED_FLAGS $DISORDERED_ROOT $DISORDERED_MNT >/dev/null
}

cr8() {
    while [ $# -gt 0 ]; do
        mkdir -p $(dirname $1)
        echo "deterministic test" >$1
        shift
    done
}

local_reset() {
    unmount_disordered
    reset_teststate
    mount_disordered
    cr8 $@
}

run_outcome() {
    local_reset $DISORDERED_MNT/a $DISORDERED_MNT/b
    $rdfind -deterministic $1 -makeclones true $DISORDERED_MNT >rdfind.out
    fdA=$(open $DISORDERED_MNT/a O_RDONLY)
    fdB=$(open $DISORDERED_MNT/b O_RDONLY)
    if [ $? -eq 0 ]; then
        if [ "$(fcntl $fdA F_LOG2PHYS 0)" = "$(fcntl $fdB F_LOG2PHYS 0)" ]; then
            outcome=a
        else
            outcome=b
        fi
        close $fdA
        close $fdB
    fi
}

trap "unmount_disordered;cleanup" INT QUIT EXIT

# Test deterministic behavior
DISORDERED_FLAGS=$DISORDERED_FLAGS_ASC
run_outcome true
outcome_asc=$outcome

DISORDERED_FLAGS=$DISORDERED_FLAGS_DESC
run_outcome true
outcome_desc=$outcome

if [ "$outcome_desc" != "$outcome_asc" ]; then
    dbgecho "fail! deterministic true should give same outcome regardless of ordering"
    exit 1
fi

dbgecho "all is good for the deterministic clone tests!"