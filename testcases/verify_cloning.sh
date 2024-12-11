#!/bin/sh
# Ensures that APFS cloning works as intended

set -e
. "$(dirname "$0")/common_funcs.sh"

# Only run on macOS/APFS
if [ "$(uname)" != "Darwin" ]; then
    echo "$me: not running on macOS, skipping APFS clone tests"
    exit 0
fi

# Compile clone checker if needed
if [ ! -f /tmp/clone_checker ]; then
    dbgecho "Compiling clone checker..."
    curl -sSL https://raw.githubusercontent.com/dyorgio/apfs-clone-checker/be8d03c9a8c5d3996582d1adfb85d9a9b230d07f/clone_checker.c -o /tmp/clone_checker.c
    if gcc /tmp/clone_checker.c -o /tmp/clone_checker; then
        dbgecho "Clone checker compiled successfully."
    else
        echo "Could not compile clone checker. Cannot proceed with tests."
        exit 1
    fi
fi

# Create test files
local_reset() {
    reset_teststate
    echo "clone test" >a
    echo "clone test" >b
}

# Basic clone operation test
local_reset
$rdfind -makeclones true a b >rdfind.out
[ -f a ]
[ -f b ]
# Check if they are clones
if [ "$(/tmp/clone_checker a b)" = "1" ]; then
    dbgecho "basic clone operation works"
else
    dbgecho "files are not clones!"
    exit 1
fi

# Test with non-APFS filesystem (if possible)
if [ -d /Volumes ]; then
    for vol in /Volumes/*; do
        if [ -d "$vol" ] && [ "$(/usr/sbin/diskutil info "$vol" | grep 'Type (Bundle):' | grep -v 'apfs')" ]; then
            local_reset
            # Found a non-APFS volume, try to use it
            if cp a "$vol/test_a" 2>/dev/null && cp b "$vol/test_b" 2>/dev/null; then
                $rdfind -makeclones true a "$vol/test_b" >rdfind.out 2>&1
                grep -q "not on APFS filesystems" rdfind.out
                rm -f "$vol/test_a" "$vol/test_b"
                dbgecho "properly handles non-APFS filesystems"
                break
            fi
        fi
    done
fi

# Test with directory hierarchies
local_reset
mkdir -p dir1/subdir dir2/subdir
echo "clone test" >dir1/subdir/a
echo "clone test" >dir2/subdir/b
$rdfind -makeclones true dir1 dir2
[ -f dir1/subdir/a ]
[ -f dir2/subdir/b ]
if [ "$(/tmp/clone_checker dir1/subdir/a dir2/subdir/b)" = "1" ]; then
    dbgecho "works with directory hierarchies"
else
    dbgecho "directory hierarchy files are not clones!"
    exit 1
fi

# Test dryrun mode
dbgecho "Starting dryrun test"
local_reset
$rdfind -dryrun true -makeclones true a b >rdfind.out
grep -q "(DRYRUN MODE) clone .* from" rdfind.out || {
    dbgecho "Dryrun message not found in output:"
    cat rdfind.out
    exit 1
}
if [ "$(/tmp/clone_checker a b)" = "1" ]; then
    dbgecho "Files are clones in dryrun mode!"
    exit 1
else
    dbgecho "Dryrun mode works correctly"
fi

# Test with existing clones
local_reset
$rdfind -makeclones true a b >rdfind.out
$rdfind -makeclones true a b >rdfind.out
grep -q "Skipped .* files that were already clones" rdfind.out
dbgecho "properly handles existing clones"

# Test with system files (should fail gracefully)
if [ "$(id -u)" -eq 0 ]; then
    dbgecho "running as root or through sudo, dangerous! Will not proceed with this part."
else
    local_reset
    system_file=$(which ls)
    cp "$system_file" .
    $rdfind -makeclones true . "$system_file" 2>&1 | tee rdfind.out
    grep -q "Failed to" rdfind.out
    [ -f "$(basename "$system_file")" ]
    dbgecho "handles permission errors gracefully"
fi

dbgecho "all is good for the clone tests!"