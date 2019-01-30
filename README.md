# AOS-Hypervisor

# Vcpu-Scheduler-and-Memory-Coordinator

vCPU scheduler and memory coordinator that dynamically manage the resources assigned to each guest machine. 

Each of these programs will be running in the host machine's user space, collecting statistics for each guest machine through hypervisor calls and taking proper actions.

During one interval, the vCPU scheduler should track each guest machine's vCPU utilization and decide how to pin them to pCPUs so that all pCPUs are WELL BALANCED, such that every pCPU handles a similar amount of workload. The "pin changes" can incur overhead, but the vCPU scheduler should try its best to minimize it.

Similarly, during one interval the memory coordinator should track each guest machine's memory utilization and decide how much extra free memory should be given to each guest machine. The memory coordinator will set the memory size of each guest machine and trigger the balloon driver to inflate and deflate. The memory coordinator should react properly when the memory resource is insufficient. 
