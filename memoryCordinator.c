#include <stdio.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <unistd.h>

#define VIR_DOMAIN_MEMORY_STAT_LAST VIR_DOMAIN_MEMORY_STAT_NR
#define deficitMultiplier 0.2
#define domMemThreshold 350 * 1024 // 350 MB
#define hostMemThreshold 400 * 1024 // 400 MB
#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })


static const double STARVATION =  0.2; // If unused < 0.2 * actual -> starving
static const double WASTE = 0.35;  // If unused > 0.35 * actual -> wasting
static const double PENALTY = 0.3; // Penalty. By which rate we are deflating the unused memory.
static const double HOST_GENEROSITY = 8.0; // One factor by which memory is released from host
static const unsigned long MIN_HOST_MEMORY = 400 * 1024; //Amount of memory needed by host to keep from crash.
static const unsigned long MIN_DOM_MEMORY = 350 * 1024; // Amount of memory needed by domain to keep from crash.
static const char DBG_PWU = 1;

typedef struct DomainList {
    virDomainPtr *domainArr;
    int nDomains;
} DomainList;


typedef struct DomainMemObj{
    virDomainPtr domain;
    unsigned long long int totalMemory;
    unsigned long long int availableMemory;
}DomainMemObj;

unsigned long long int convert(unsigned long long int val){
    return val/1024;
}

int getDomains(virConnectPtr conn, struct DomainList *domainList) {
    virDomainPtr *domainArr;

    int numberDomains = virConnectListAllDomains(conn, &domainArr, VIR_CONNECT_LIST_DOMAINS_ACTIVE |
                         VIR_CONNECT_LIST_DOMAINS_RUNNING);
    if (numberDomains == 0) {
        printf("No active domains!\n");
        exit(EXIT_FAILURE);
    }
    domainList->domainArr = domainArr;
    domainList->nDomains = numberDomains;
    printf("we have %d active domains\n", numberDomains);
    
    return numberDomains;
}

unsigned long long int freeSurplusMem(DomainMemObj *memorySurplusDomain, int memSurplusDoms){
    unsigned long long int freedMem = 0;
    for(int j=0; j<memSurplusDoms; j++){
        unsigned long long int newAlloc = memorySurplusDomain[j].totalMemory - (0.3 * memorySurplusDomain[j].availableMemory);
        newAlloc = (newAlloc < domMemThreshold ? domMemThreshold : newAlloc);
        freedMem += (memorySurplusDomain[j].totalMemory  - newAlloc);
        // virDomainSetMemory(memorySurplusDomain[j].domain, newAlloc);
        printf("\t-> Domain: %s, Deflating memory from %llu MB to %llu MB.\n",virDomainGetName(memorySurplusDomain[j].domain), convert(memorySurplusDomain[j].totalMemory), convert(newAlloc));
        virDomainSetMemory(memorySurplusDomain[j].domain, newAlloc);
    }
    // printf("Total Memory Freed: %llu MB\n", convert(freedMem));
    return freedMem;
}

void printMemoryDetails(DomainMemObj *memoryDeficitDomain, int memDeficitDoms, DomainMemObj *memorySurplusDomain, int memSurplusDoms){

    printf("\n------------------------- Domain Memory Details---------------------------------------------------------\n");
    printf("Memory Deficit Domains:\n");
    for(int i=0; i<memDeficitDoms; i++){
        printf("Domain: %s \t Total Memory: %llu \t Available Memory: %llu\n", virDomainGetName(memoryDeficitDomain[i].domain), memoryDeficitDomain[i].totalMemory/1024, memoryDeficitDomain[i].availableMemory/1024);        
    }
    printf("Memory Surplus Domains:\n");
    for(int i=0; i<memSurplusDoms; i++){
        printf("Domain: %s \t Total Memory: %llu \t Available Memory: %llu\n", virDomainGetName(memorySurplusDomain[i].domain), memorySurplusDomain[i].totalMemory/1024, memorySurplusDomain[i].availableMemory/1024);        
    }
    printf("-----------------------------------------------------------------------------------------------------------\n\n");
}

unsigned long long int findNeed(DomainMemObj *memoryDeficitDomain, int memDeficitDoms){
    unsigned long long int total = 0;
    for(int i=0;i<memDeficitDoms; i++){
        unsigned long long int temp = (deficitMultiplier * memoryDeficitDomain[i].totalMemory) - memoryDeficitDomain[i].availableMemory;
        total += temp;
    }
    return total;
}

unsigned long long int freeHostMem(unsigned long long int totalMemNeeded, unsigned long long int totalMemFreed, virConnectPtr conn){
    unsigned long long int stillRequired = 2.0 * (totalMemNeeded - totalMemFreed);
    unsigned long long int hostMemFreed = convert(virNodeGetFreeMemory(conn));
    if(hostMemFreed < hostMemThreshold){
        printf("\t-> Host can't spare any memory, host is sorry!!\n");
        return 0;
    }
    else{
        stillRequired = min(stillRequired, hostMemFreed);
        printf("\t-> Host was able to cough up %llu MB\n", convert(stillRequired));
        return stillRequired;
    }
    
} 

void allocateFreedMem(DomainMemObj *memoryDeficitDomain, int memDeficitDoms, unsigned long long int memAlloc){
    for(int i=0;i<memDeficitDoms;i++){
        memAlloc = min(memAlloc + memoryDeficitDomain[i].totalMemory, virDomainGetMaxMemory(memoryDeficitDomain[i].domain));
        printf("\t-> Starving domain %s: Inflating Memory from %llu MB to %llu MB.\n", virDomainGetName(memoryDeficitDomain[i].domain),  
            convert(memoryDeficitDomain[i].totalMemory), 
            convert(memAlloc));
        virDomainSetMemory(memoryDeficitDomain[i].domain, memAlloc);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf(" Error - Usage: ./vcpu_scheduler <time in seconds>\n");
        return 1;
    }
    int time = atoi(argv[1]);
    // Connect to Hypervisor
    virConnectPtr conn = virConnectOpen("qemu:///system");

    DomainList* domainList = (DomainList *)calloc(1, sizeof(DomainList));
    if(domainList == NULL){
        printf("Error - something went wrong with allocating memory\n");
        return(1);
    } 

    while(1){
        int nDomains = getDomains(conn, domainList);
        if(nDomains <= 0){
            break;
        }
        // Figure out memory Deficit & memory surplus domains
        DomainMemObj *memoryDeficitDomain = (DomainMemObj *)calloc(nDomains, sizeof(DomainMemObj));
        DomainMemObj *memorySurplusDomain = (DomainMemObj *)calloc(nDomains, sizeof(DomainMemObj));
        int memDeficitDoms = 0, memSurplusDoms = 0;
        for(int i=0;i<nDomains;i++){
            virDomainPtr currDomain = domainList->domainArr[i];
            /* int virDomainSetMemoryStatsPeriod   (virDomainPtr domain, int period, unsigned int flags);
            Dynamically change the domain memory balloon driver statistics collection period. 
            Use 0 to disable and a positive value to enable.

            @flags may include VIR_DOMAIN_AFFECT_LIVE or VIR_DOMAIN_AFFECT_CONFIG. Both flags may be set. 
            If VIR_DOMAIN_AFFECT_LIVE is set, the change affects a running domain and will fail if domain is not active. 
            If VIR_DOMAIN_AFFECT_CONFIG is set, the change affects persistent state, and will fail for transient domains. 
            If neither flag is specified (that is, @flags is VIR_DOMAIN_AFFECT_CURRENT), then an inactive domain modifies persistent setup, 
            while an active domain is hypervisor-dependent on whether just live or both live and persistent state is changed.
            Not all hypervisors can support all flag combinations.
            domain: a domain object or NULL
            period: the period in seconds for stats collection
            flags: bitwise-OR of virDomainMemoryModFlags
            Returns 0 in case of success, -1 in case of failure. */

            int changePeriodVal = virDomainSetMemoryStatsPeriod(currDomain, 1, VIR_DOMAIN_AFFECT_CURRENT);
            if(changePeriodVal == -1){
                printf("Could not change memory balloon driver statistics collection period for domain %s\n", virDomainGetName(currDomain));
            }
               /*This function provides memory statistics for the domain.
            Up to 'nr_stats' elements of 'stats' will be populated with memory statistics from the domain. 
            Only statistics supported by the domain, the driver, and this version of libvirt will be returned.

            Memory Statistics:

            
VIR_DOMAIN_MEMORY_STAT_LAST =   VIR_DOMAIN_MEMORY_STAT_NR
VIR_DOMAIN_MEMORY_STAT_SWAP_IN  =   0   The total amount of data read from swap space (in kB).
VIR_DOMAIN_MEMORY_STAT_SWAP_OUT =   1   The total amount of memory written out to swap space (in kB).
VIR_DOMAIN_MEMORY_STAT_MAJOR_FAULT  =   2   Page faults occur when a process makes a valid access to virtual memory that is not available. 
                                            When servicing the page fault, if disk IO is required, it is considered a major fault. 
                                            If not, it is a minor fault. These are expressed as the number of faults that have occurred.
VIR_DOMAIN_MEMORY_STAT_MINOR_FAULT  =   3
VIR_DOMAIN_MEMORY_STAT_UNUSED   =   4   The amount of memory left completely unused by the system. 
                                        Memory that is available but used for reclaimable caches should NOT be reported as free. 
                                        This value is expressed in kB.
VIR_DOMAIN_MEMORY_STAT_AVAILABLE    =   5   The total amount of usable memory as seen by the domain. 
                                            This value may be less than the amount of memory assigned to the domain 
                                            if a balloon driver is in use or if the guest OS does not initialize all assigned pages. 
                                            This value is expressed in kB.
VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON   =   6   Current balloon value (in KB).
VIR_DOMAIN_MEMORY_STAT_RSS  =   7   Resident Set Size of the process running the domain. This value is in kB
VIR_DOMAIN_MEMORY_STAT_USABLE   =   8   How much the balloon can be inflated without pushing the guest system to swap, corresponds to 'Available' in /proc/meminfo
VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE  =   9   Timestamp of the last update of statistics, in seconds.
VIR_DOMAIN_MEMORY_STAT_DISK_CACHES  =   10  The amount of memory, that can be quickly reclaimed without additional I/O (in kB). Typically these pages are used for caching files from disk.
VIR_DOMAIN_MEMORY_STAT_NR   =   11  The number of statistics supported by this version of the interface. To add new statistics, add them to the enum and increase this value.

            dom: pointer to the domain object
            stats: nr_stats-sized array of stat structures (returned)
            nr_stats: number of memory statistics requested
            flags: extra flags; not used yet, so callers should always pass 0
            Returns: The number of stats provided or -1 in case of failure. */
            virDomainMemoryStatStruct memInfo[VIR_DOMAIN_MEMORY_STAT_LAST];
            int getDomMemStatsResult = virDomainMemoryStats(currDomain, memInfo, VIR_DOMAIN_MEMORY_STAT_LAST, 0);
            if(getDomMemStatsResult == -1){
                printf("Could not fetch memory statistics for domain %s\n", virDomainGetName(currDomain));
            }

            unsigned long long int totalDomainMemory, unusedMemory;
            for(int j=0; j<VIR_DOMAIN_MEMORY_STAT_LAST; j++){
                switch(memInfo[j].tag){
                    case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                        unusedMemory = memInfo[j].val;
                        break;
                    case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                        totalDomainMemory = memInfo[j].val;
                        break;
                }
            }
            printf("Domain: %s, Total Domain Memory: %llu MB, Total Unsed Memory: %llu MB\n", virDomainGetName(currDomain), convert(totalDomainMemory), convert(unusedMemory)); 
            if( unusedMemory < deficitMultiplier * totalDomainMemory){
                // memoryDeficitDomain
                memoryDeficitDomain[memDeficitDoms].domain = currDomain;
                memoryDeficitDomain[memDeficitDoms].totalMemory = totalDomainMemory;
                memoryDeficitDomain[memDeficitDoms].availableMemory = unusedMemory;
                memDeficitDoms++;

            }
            else if( unusedMemory > 0.35 * totalDomainMemory){
                //memorySurplusDomain
                memorySurplusDomain[memSurplusDoms].domain = currDomain;
                memorySurplusDomain[memSurplusDoms].totalMemory = totalDomainMemory;
                memorySurplusDomain[memSurplusDoms].availableMemory = unusedMemory;
                memSurplusDoms++;

            }
        }
        printMemoryDetails(memoryDeficitDomain, memDeficitDoms, memorySurplusDomain, memSurplusDoms);
        printf("----------------------------------------  Transactions ------------------------------------------------------------\n");
        if(memSurplusDoms != 0 && memDeficitDoms == 0){
            printf("* No Starving Domains, only Memory Surplus Domains. Giving memory back to Hypervisor\n");
            // No deficit domains, only surplus domains. Give memory back to host
            unsigned long long int temp = freeSurplusMem(memorySurplusDomain, memSurplusDoms);
            printf("* Total Memory Freed: %llu MB\n", convert(temp));
        }
        else if(memDeficitDoms > 0){
            unsigned long long int totalMemNeeded = findNeed(memoryDeficitDomain,memDeficitDoms);
            printf("* Starving domains exist, memory needed for them is %llu MB\n", convert(totalMemNeeded));
            unsigned long long int totalMemFreed = 0;
            if(memSurplusDoms > 0)
                totalMemFreed += freeSurplusMem(memorySurplusDomain, memSurplusDoms);

            if(totalMemNeeded > totalMemFreed){
                // Memory still needed, Free Memory from Host
                totalMemFreed += freeHostMem(totalMemNeeded, totalMemFreed, conn);
            }
            printf("-Total Memory Freed: %llu MB\n", convert(totalMemFreed));
            unsigned long long int memAlloc = totalMemFreed / memDeficitDoms;
            allocateFreedMem(memoryDeficitDomain, memDeficitDoms, memAlloc);

        }
        printf("---------------------------------------------------------------------------------------------------------------------\n");
        printf("Iteration done...\n\n");        
        sleep(time);
    }
    printf("Exiting..");
    virConnectClose(conn);
    return 0;
}