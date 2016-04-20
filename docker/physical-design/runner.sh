#!/bin/bash
set -e

#
# Example script
#
# name=all
# runtime=10
# pg_nums="128"
# stripe_widths="5"
# queue_depths="4"
# entry_sizes="4096"
# pool=zlog
#
# # workloads
# wl_11="map_11 bytestream_11"
# wl_n1="map_n1 bytestream_n1_write bytestream_n1_append"
# workloads="$wl_11 $wl_n1"
#
# # i/o interfaces
# map_n1_if="vanilla cls_no_index cls_no_index_wronly cls_full"
# bytestream_n1_write_if="vanilla cls_no_index cls_no_index_wronly cls_full cls_full_hdr_idx cls_full_inline_idx"
# bytestream_n1_append_if="vanilla cls_no_index cls_no_index_wronly cls_check_epoch cls_check_epoch_hdr cls_full cls_full_hdr_idx cls_no_index_wronly_xtn"
#
# . runner.sh
# run_pd
#

# name of results dir
logdir=$PWD/results.pd.${name}.$(hostname --short).$(date +"%m-%d-%Y_%H-%M-%S")
mkdir $logdir

this_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

function run_pd() {

# log file dir in container
guest_logdir=/results

for pgnum in $pg_nums; do
for stripe_width_want in $stripe_widths; do
for qdepth in $queue_depths; do
for entry_size in $entry_sizes; do
for workload in $workloads; do

if [ "$workload" = "map_n1" ]; then
  interfaces=$map_n1_if
elif [ "$workload" = "bytestream_n1_write" ]; then
  interfaces=$bytestream_n1_write_if
elif [ "$workload" = "bytestream_n1_append" ]; then
  interfaces=$bytestream_n1_append_if
else
  interfaces="vanilla"
fi

for interface in $interfaces; do

if [ "$workload" = "map_11" ] || [ "$workload" = "bytestream_11" ]; then
  stripe_width=0
else
  stripe_width=$stripe_width_want
fi

ename="pool-${pool}_expr-${workload}"
ename="${ename}_sw-${stripe_width}"
ename="${ename}_es-${entry_size}"
ename="${ename}_qd-${qdepth}"
ename="${ename}_pg-${pgnum}"
ename="${ename}_rt-${runtime}"
ename="${ename}_if-${interface}"

set -x

# if reset is set to 'soft' then we will nuke the ceph installation before
# proceeding. in either case the experiment will ensure it is using a fresh
# pool configured according to the experiment parameters.
if [ "x$reset" = "xsoft" ]; then
  ${this_dir}/single-node-ceph.sh --data-dev ${data_dev} --noop ${data_dev}
fi

docker run --net=host \
  -v $logdir:$guest_logdir \
  -v /etc/ceph:/etc/ceph \
  -it zlog-pd \
  --pool $pool \
  --pgnum $pgnum \
  --workload $workload \
  --stripe-width $stripe_width \
  --entry-size $entry_size \
  --queue-depth $qdepth \
  --runtime $runtime \
  --interface $interface \
  --output $guest_logdir/$ename \
  --rest $rest

set +x

done
done
done
done
done
done

}
