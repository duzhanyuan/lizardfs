CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_lizardfs info

dir="${info[mount0]}/dir"
mkdir "$dir"
mfssetgoal xor2 "$dir"

# Create a temporary file with some data
file_size_mb=5
tmpf=$RAMDISK_DIR/tmpf
FILE_SIZE=${file_size_mb}M file-generate "$tmpf"

# Create a file on LizardFS filesystem with random data
dd if=/dev/urandom of="$dir/file" bs=1MiB count=$file_size_mb

# Overwirte the file using data from the temporary file
# Use 20 parallel threads, each of them overwrites a random 1 KB block
seq 0 $((file_size_mb*1024-1)) | shuf | xargs -P20 -IXX \
		dd if="$tmpf" of="$dir/file" bs=1024 count=1 seek=XX skip=XX conv=notrunc status=none

# Validate in the usual way
if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file is different than written"
fi

# Find the chunkserver serving part 1 of 2 and stop it
csid=$(find_first_chunkserver_with_chunks_matching 'chunk_xor_1_of_2*')
mfschunkserver -c "${info[chunkserver${csid}_config]}" stop

# Validate the parity part
if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file (with broken chunkserver) is different than written"
fi