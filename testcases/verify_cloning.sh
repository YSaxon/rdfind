# verify_cloning.sh
#!/bin/sh
# Ensures that APFS cloning works as intended

set -e
. "$(dirname "$0")/common_funcs.sh"

# Only run on macOS/APFS
if [ "$(uname)" != "Darwin" ]; then
    echo "$me: not running on macOS, skipping APFS clone tests"
    exit 0
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
# Check if they are clones using F_LOG2PHYS
fdA=$(open a O_RDONLY)
fdB=$(open b O_RDONLY)
if [ $? -eq 0 ]; then
    blockA=$(fcntl $fdA F_LOG2PHYS 0)
    blockB=$(fcntl $fdB F_LOG2PHYS 0)
    [ "$blockA" = "$blockB" ]
    close $fdA
    close $fdB
fi
dbgecho "basic clone operation works"

# Test with non-APFS filesystem
local_reset
mkdir tmpfs
mount -t tmpfs none tmpfs
cp a tmpfs/a
cp b tmpfs/b
$rdfind -makeclones true a tmpfs/b >rdfind.out 2>&1
grep -q "not on APFS filesystems" rdfind.out
umount tmpfs
dbgecho "properly handles non-APFS filesystems"

# Test with directory hierarchies
local_reset
mkdir -p dir1/subdir dir2/subdir
echo "clone test" >dir1/subdir/a
echo "clone test" >dir2/subdir/b
$rdfind -makeclones true dir1 dir2
[ -f dir1/subdir/a ]
[ -f dir2/subdir/b ]
fdA=$(open dir1/subdir/a O_RDONLY)
fdB=$(open dir2/subdir/b O_RDONLY)
if [ $? -eq 0 ]; then
    blockA=$(fcntl $fdA F_LOG2PHYS 0)
    blockB=$(fcntl $fdB F_LOG2PHYS 0)
    [ "$blockA" = "$blockB" ]
    close $fdA
    close $fdB
fi
dbgecho "works with directory hierarchies"

# Test dryrun mode
local_reset
$rdfind -dryrun true -makeclones true a b >rdfind.out
grep -q "(DRYRUN MODE) Would clone" rdfind.out
fdA=$(open a O_RDONLY)
fdB=$(open b O_RDONLY)
if [ $? -eq 0 ]; then
    blockA=$(fcntl $fdA F_LOG2PHYS 0)
    blockB=$(fcntl $fdB F_LOG2PHYS 0)
    [ "$blockA" != "$blockB" ]
    close $fdA
    close $fdB
fi
dbgecho "dryrun mode works correctly"

# Test with existing clones
local_reset
$rdfind -makeclones true a b >rdfind.out
$rdfind -makeclones true a b >rdfind.out
grep -q "Skipped .* files that were already clones" rdfind.out
dbgecho "properly handles existing clones"

# Test with system files (should fail gracefully)
local_reset
system_file=$(which ls)
cp $system_file .
if [ "$(id -u)" -eq 0 ]; then
    dbgecho "running as root or through sudo, dangerous! Will not proceed with this part."
else
    $rdfind -makeclones true . $system_file 2>&1 | tee rdfind.out
    grep -q "Failed to" rdfind.out
    [ -f $(basename $system_file) ]
    dbgecho "handles permission errors gracefully"
fi

dbgecho "all is good for the clone tests!"