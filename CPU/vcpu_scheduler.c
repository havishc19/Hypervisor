#include <stdio.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <float.h>

static const double DIFF_THRESHOLD = 20.0;

typedef struct DomainStats {
    virDomainPtr domain;
    int nVCPU;
    unsigned long long *vcpuTimeArr;
    int *pcpuArr;
} DomainStats;



void initializeDomainStats(int nPCPU, virDomainPtr* domainList, DomainStats* currDomainStats, int nDomains){
    /*struct virVcpuInfo {
        unsigned int number: virtual CPU number
        int state: value from virVcpuState
        unsigned long long  cpuTime: CPU time used, in nanoseconds
        int cpu: real CPU number, or -1 if offline
    }*/
    /* typedef virVcpuInfo * virVcpuInfoPtr */
    virVcpuInfoPtr vcpuInfo;
    size_t cpuMapSize;
    unsigned char *cpuMaps;
    for(int i=0;i<nDomains; i++){

        /*int virDomainGetVcpusFlags (virDomainPtr domain, unsigned int flags);
        Query the number of virtual CPUs used by the domain. Note that this call may fail if the underlying virtualization 
        hypervisor does not support it. This function may require privileged access to the hypervisor.

        If @flags includes VIR_DOMAIN_AFFECT_LIVE, this will query a running domain (which will fail if domain is not active); 
        if it includes VIR_DOMAIN_AFFECT_CONFIG, this will query the XML description of the domain. It is an error to set both flags. 
        If neither flag is set (that is, VIR_DOMAIN_AFFECT_CURRENT), then the configuration queried depends on whether
         the domain is currently running.

        If @flags includes VIR_DOMAIN_VCPU_MAXIMUM, then the maximum virtual CPU limit is queried. Otherwise, 
        this call queries the current virtual CPU count.

        If @flags includes VIR_DOMAIN_VCPU_GUEST, then the state of the processors is queried in the guest instead of the hypervisor. 
        This flag is only usable on live domains. Guest agent may be needed for this flag to be available. */
        int nVCPU = virDomainGetVcpusFlags(domainList[i], VIR_DOMAIN_AFFECT_LIVE);
        if(nVCPU == -1){
            printf("Error: Domain Not Active!\n");
            exit(EXIT_FAILURE);
        }
        currDomainStats[i].vcpuTimeArr = (unsigned long long *)calloc(nVCPU, sizeof(unsigned long long));
        currDomainStats[i].pcpuArr = (int *)calloc(nVCPU, sizeof(int));
        vcpuInfo = (virVcpuInfoPtr)calloc(nVCPU, sizeof(virVcpuInfo));
        /* VIR_CPU_MAPLEN This macro is to be used in conjunction with virDomainPinVcpu() API. It returns the length (in bytes) required 
        to store the complete CPU map between a single virtual & all physical CPUs of a domain. */
        cpuMapSize = VIR_CPU_MAPLEN(nPCPU);
        cpuMaps = (unsigned char*)calloc(nVCPU, cpuMapSize);
        /* int virDomainGetVcpus (virDomainPtr domain, virVcpuInfoPtr info, int maxinfo, unsigned char * cpumaps, int maplen) 
        Extract information about virtual CPUs of domain, store it in info array and also in cpumaps 
        if this pointer isn't NULL. This call may fail on an inactive domain.
        
        domain: pointer to domain object, or NULL for Domain0
        info: pointer to an array of virVcpuInfo structures (OUT)
        maxinfo: number of structures in info array
        cpumaps: pointer to a bit map of real CPUs for all vcpus of this domain (in 8-bit bytes) (OUT) If cpumaps is NULL, then no cpumap information is returned by the API. It's assumed there is <maxinfo> cpumap in cpumaps array. The memory allocated to cpumaps must be (maxinfo * maplen) bytes (ie: calloc(maxinfo, maplen)). One cpumap inside cpumaps has the format described in virDomainPinVcpu() API.
        maplen: number of bytes in one cpumap, from 1 up to size of CPU map in underlying virtualization system (Xen...). Must be zero when cpumaps is NULL and positive when it is non-NULL.
        Returns: the number of info filled in case of success, -1 in case of failure. 
        
        cpumap: pointer to a bit map of real CPUs (in 8-bit bytes) (IN) Each bit set to 1 means that corresponding CPU is usable.
         Bytes are stored in little-endian order: CPU0-7, 8-15... In each byte, lowest CPU number is least significant bit.

        */
        int temp = virDomainGetVcpus(domainList[i], vcpuInfo, nVCPU, cpuMaps, cpuMapSize);
        if(temp == -1){
            printf("Error\n");
            exit(EXIT_FAILURE);
        }
        int j=0;
        while(j < nVCPU){
            currDomainStats[i].vcpuTimeArr[j] = vcpuInfo[j].cpuTime;
            currDomainStats[i].pcpuArr[j] = vcpuInfo[j].cpu;
            j++;
        }
        currDomainStats[i].nVCPU = nVCPU;
        currDomainStats[i].domain = domainList[i];
    }
    return;
}

int find(double *pcpuUsage, int nPCPU, int *maxUtilCpu, int *minUtilCpu){
    double max = 0.0;
    double min = DBL_MAX;

    for(int i=0; i < nPCPU; i++){
        double temp = pcpuUsage[i];
        if(temp > max){
            max = temp;
            *maxUtilCpu = i;
        }
        if(temp < min){
            min = temp;
            *minUtilCpu = i;
        }
    }
    if(max - min <= 20.0){
        return 0;
    }
    return 1;
}


void printUsage(double *pcpuUsage, int nPCPU){
    printf("---------- pCPU Usage --------------- \n");
    for(int i=0;i<nPCPU;i++){
        printf("\t->pCPU: %d, Usage: %f\n", i, pcpuUsage[i]);
    }
    return;
}

void affinityPinning(int nPCPU, int nDomains, DomainStats *currDomainStats){
    size_t cpuMapSize = VIR_CPU_MAPLEN(nPCPU);
    unsigned char cpuMap = 0x1;
    for(int i=0;i<nDomains;i++){
        for(int j=0;j<currDomainStats[i].nVCPU;j++){
            int realCPU = currDomainStats[i].pcpuArr[j];
            cpuMap = 0x1 << realCPU;
            virDomainPinVcpu(currDomainStats[i].domain, j, &cpuMap, cpuMapSize);
        }
    }
}

void pinVcpu(double *pcpuUsage, int nPCPU, DomainStats *currDomainStats, DomainStats *prevDomainStats, int nDomains, double timePeriod){
    int maxUtilCpu, minUtilCpu;

    printUsage(pcpuUsage, nPCPU);
    // Find CPUs which are utilized max and min
    int result = find(pcpuUsage, nPCPU, &maxUtilCpu, &minUtilCpu);
    printf("Lightest pCPU: %d, Heaviest pCPU: %d\n", minUtilCpu, maxUtilCpu);
    printf("----------- Pinning vCPU Transactions ----------------------\n");

    if(result == 0){
        printf("\t->Load distributed equally, no need to balance load \n");
        affinityPinning(nPCPU, nDomains, currDomainStats);
        return;
    }

    // Take Lightest vCPU from maxUtilCPU and assign it minUtilCpu
    size_t cpuMapSize = VIR_CPU_MAPLEN(nPCPU);
    unsigned char minUtilCpuMap = 0x1 << minUtilCpu;
    double leastWtvCPU = DBL_MAX;
    virDomainPtr leastWtvCpuDomain;
    int leastWtvCpuNumber;
    for(int i=0;i<nDomains;i++){
        for(int j=0; j<currDomainStats[i].nVCPU; j++){
            int realCPU = currDomainStats[i].pcpuArr[j];
            // Finding the least wt vCPU in maxUtilpCPU
            if(realCPU == maxUtilCpu){
                double tempTime = ((currDomainStats[i].vcpuTimeArr[j] - prevDomainStats[i].vcpuTimeArr[j]) * 100.0)/ (timePeriod);
                if(tempTime < leastWtvCPU){
                    leastWtvCPU = tempTime;
                    // Record Domain of vCPU & vCPU Number;
                    leastWtvCpuDomain = currDomainStats[i].domain;
                    leastWtvCpuNumber = j;
                }
            }
        }
    }
    printf("\t->Pinning vCPU %d in Domain %s from %d pCPU to pCPU %d \n", leastWtvCpuNumber,
           virDomainGetName(leastWtvCpuDomain), maxUtilCpu ,minUtilCpu);
    int pinResult = virDomainPinVcpu(leastWtvCpuDomain, leastWtvCpuNumber, &minUtilCpuMap, cpuMapSize);
    if( pinResult == 0){
        printf("\t->Pin Result: %s\n", pinResult == 0 ? "Success" : "Failure");
    }

    return;

}

void calculateCpuMetrics(int nDomains, DomainStats *currDomainStats, DomainStats *prevDomainStats, double *pcpuUsage, double timePeriod){
    printf("----- CPU Metrics --- \n");
    for(int i=0; i<nDomains; i++){
        for(int j=0; j<currDomainStats[i].nVCPU; j++){
            double tempTime = ((currDomainStats[i].vcpuTimeArr[j] - prevDomainStats[i].vcpuTimeArr[j]) * 100.0)/ (timePeriod);
            printf("\t-> Domain: %s, vCPU: %d, pCPU: %d, Usage: %f \n", virDomainGetName(currDomainStats[i].domain), j, 
                                                                    currDomainStats[i].pcpuArr[j], tempTime);
            pcpuUsage[currDomainStats[i].pcpuArr[j]] += tempTime;
        }
    }
}

int getDomains(virConnectPtr conn, virDomainPtr **domainList) {
    virDomainPtr *domainArr;

    int numberDomains = virConnectListAllDomains(conn, &domainArr, VIR_CONNECT_LIST_DOMAINS_ACTIVE |
                         VIR_CONNECT_LIST_DOMAINS_RUNNING);
    if (numberDomains == 0) {
        printf("No active domains!\n");
        exit(EXIT_FAILURE);
    }
    *domainList = domainArr;
    return numberDomains;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf(" Error - Usage: ./vcpu_scheduler <time in seconds>\n");
        return 1;
    }
    int time = atoi(argv[1]);
    // Connect to Hypervisor
    virConnectPtr conn = virConnectOpen("qemu:///system");

    // Find the number of PCPUs
    /*struct virNodeInfo {
        char model[32]  model   
        string indicating the CPU model
        unsigned long   memory  
        memory size in kilobytes
        unsigned int    cpus    
        the number of active CPUs
        unsigned int    mhz 
        expected CPU frequency, 0 if not known or on unusual architectures
        unsigned int    nodes   
        the number of NUMA cell, 1 for unusual NUMA topologies or uniform memory access; check capabilities XML for the actual NUMA topology
        unsigned int    sockets 
        number of CPU sockets per node if nodes > 1, 1 in case of unusual NUMA topology
        unsigned int    cores   
        number of cores per socket, total number of processors in case of unusual NUMA topolog
        unsigned int    threads 
        number of threads per core, 1 in case of unusual numa topology
    }*/
    virNodeInfo info;
    /*virNodeGetInfo
    int virNodeGetInfo (virConnectPtr conn, virNodeInfoPtr info);
        Extract hardware information about the node.

        conn:
        pointer to the hypervisor connection
        info:
        pointer to a virNodeInfo structure allocated by the user
        Returns:
        0 in case of success and -1 in case of failure. */
    virNodeGetInfo(conn, &info);
    int nPCPU = info.cpus;
    // Done

    // Initializing stuff
    // DomainList* domainList = (DomainList *)calloc(1, sizeof(DomainList)); 
    
    double *pcpuUsage = (double *)calloc(nPCPU, sizeof(double)); 

    if(pcpuUsage == NULL){
        printf("Error - something went wrong with allocating memory\n");
        return(1);
    }

    // Initialize & Get vCPU Stats

    DomainStats *currDomainStats, *prevDomainStats; 
    double timePeriod = atof(argv[1]) * 1000000000;
    int prevNumberDomains = 0;
    int iter = 1;
    while (1) { 
        virDomainPtr *domainList = (virDomainPtr *)malloc(sizeof(virDomainPtr));
        int nDomains = getDomains(conn, &domainList);
        if(nDomains <= 0){
            break;
        }
        printf("------- General Stats --------\n");
        printf("* n_pcpus: %d \n", nPCPU);
        printf("* Number of domains: %d \n", nDomains);
        // Initialize the Current Domain Stats Data
        currDomainStats = (DomainStats *)calloc( nDomains, sizeof(DomainStats));
        initializeDomainStats(nPCPU, domainList, currDomainStats, nDomains);

        // Check if number of active domains has changed
        if(nDomains != prevNumberDomains){
            prevDomainStats = (DomainStats *)calloc(nDomains, sizeof(DomainStats));
        }
        else{
            calculateCpuMetrics(nDomains, currDomainStats, prevDomainStats, pcpuUsage, timePeriod);
            pinVcpu(pcpuUsage, nPCPU, currDomainStats, prevDomainStats, nDomains, timePeriod);
        }
        
        //Copy Curr to Prev
        prevNumberDomains = nDomains;
        memcpy(prevDomainStats, currDomainStats, nDomains * sizeof(DomainStats));
        printf("Iteration %d done!!\n", iter);
        printf("=================================================================================\n\n\n");
        iter++;
        for(int i=0;i<nPCPU;i++){
            pcpuUsage[i] = 0.0;
        }
        sleep(time);
    } 

    virConnectClose(conn);
    printf("Done!!!\n");
    return 0;
}