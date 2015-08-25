#pragma once

// This uses mpi.h which requires the Microsoft MPI SDK to be installed on Windows
// and the MPI dev package on Linux (sudo apt-get install libopenmpi-dev openmpi-bin openmpi-doc)
#include "mpi.h"
#pragma comment(lib, "msmpi.lib")

#include <string>
#include <array>
#include <vector>

namespace Microsoft { namespace MSR { namespace CNTK {

    struct MpiFail : public std::string 
    {
        MpiFail(const std::string & what)
            : std::string (what) 
        {
        }
    };

    static int operator||(int rc, const MpiFail & what)
    {
        if (rc == MPI_SUCCESS)
        {
            return rc;
        }

        fprintf(stderr, "%s, MPI error %d\n", what.c_str(), rc); 
        fflush(stderr);

        // (special case: we use that code to indicate a missing msmpi.dll...)
        if (rc != MPI_ERR_INTERN)
        {
            char errbuf[MPI_MAX_ERROR_STRING + 1] = { 0 };
            int len;
            MPI_Error_string(rc, &errbuf[0], &len);
            fprintf(stderr, "%s, MPI error %d: %s\n", what.c_str(), rc, errbuf); 
            fflush(stderr);

            // we abort through this, so that the MPI system gets the memo
            MPI_Abort(MPI_COMM_WORLD, rc);

            // TODO: or does that only signal an issue, and we should still terminate ourselves?
            // BUGBUG: We'd also need to Abort through the other sub-set communicator
        }
        throw std::runtime_error(what);
    }

    class MPIWrapper
    {
        int m_myRank;
        int m_numMPINodes;
        size_t m_numNodesInUse;

        // MPI communicator that reflects the current subset selection
        MPI_Comm m_currentComm;   

        // MPI_Init() with delay-loading the msmpi.dll (possibly causing a failure if missing; we want to catch that)
        int MPI_Init_DL()       
        {
    #ifdef WIN32
            __try
    #endif
            {
                int argc = 0;
                char**argv = NULL;
                int provided;
                return MPI_Init_thread(&argc, &argv, MPI_THREAD_SERIALIZED, &provided);
            }
    #ifdef WIN32
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                fprintf(stderr, "mpihelper: msmpi.dll missing\n");
                return MPI_ERR_INTERN;
            }
    #endif
        }

    public:
        MPIWrapper() 
            : m_currentComm(MPI_COMM_WORLD)
        {
            static bool initialized = false;
            if (initialized)
            {
                throw std::logic_error("MPIWrapper: this is a singleton class that can only be instantiated once per process");
            }

            initialized = true;
            fprintf(stderr, "MPIWrapper: initializing MPI\n"); 
            fflush(stderr);

            MPI_Init_DL() || MpiFail("mpiaggregator: MPI_Init");
            MPI_Comm_rank(MPI_COMM_WORLD, &m_myRank);
            MPI_Comm_size(MPI_COMM_WORLD, &m_numMPINodes);
            m_numNodesInUse = m_numMPINodes;

            // by default we use all of them
            RequestNodes("MPIWrapper");

            if (m_numMPINodes > 1)
                fprintf(stderr, "mpihelper: we are cog %d in a gearbox of %d\n", (int) m_myRank, (int) m_numMPINodes);
            else
                fprintf(stderr, "mpihelper: only one MPI process: MPI operation will be boring\n");

             fflush(stderr);

             // do an initial handshake
             Ping("mpihelper");

             // stagger the jobs just a little to get a sort-of deterministic order e.g. in GPU allocation when running on one machine
             // continue 0.5 seconds apart
             ::Sleep((DWORD)(500 * CurrentNodeRank()));
        }

        // Note: we don't clear the sub-communication here although we should, because in case of a crash, this prevents the EXE from terminating.
        // It's OK since this class is a singleton anyway that gets instantiated exactly once at program startup.
        ~MPIWrapper() 
        {
            fprintf(stderr, "~MPIWrapper\n"); 
            fflush(stderr);
            MPI_Finalize(); 
        }

        void Ping(const char * msg) const
        {
    #undef USE2NDCOMM
    #ifndef USE2NDCOMM
             if (NumNodesInUse() != m_numMPINodes)
             {
                 fprintf(stderr, "ping [%s]: cannot be applied to subset (%d) of nodes, skipping\n", msg, (int)NumNodesInUse());
                 fflush(stderr);
                 return;
             }
    #endif
             std::array<int,1> handshake;
             handshake[0] = 1;

             fprintf(stderr, "ping [%s]: %d nodes pinging each other\n", msg, (int)NumNodesInUse());
             fflush(stderr);

             AllReduce(handshake);
             fprintf(stderr, "ping [%s]: all %d nodes responded\n", msg, handshake[0]); 
             fflush(stderr);
        }

        void RequestNodes(const char * msg, size_t requestednodes = SIZE_MAX/*default: all*/)
        {
            Ping("requestnodes (before change)");

            // undo current split
    #ifdef USE2NDCOMM
            if (m_currentComm != MPI_COMM_WORLD/*no subset*/ && m_currentComm != MPI_COMM_NULL/*idle nodes*/)
            {
                fprintf(stderr, "requestnodes: MPI_Comm_free %x\n", (int) m_currentComm); 
                fflush(stderr);
                MPI_Comm_free(&m_currentComm) || MpiFail("requestnodes: MPI_Comm_free");    // will leave MPI_COMM_NULL here
            }
    #endif
            // reset to MPI_COMM_WORLD
            m_currentComm = MPI_COMM_WORLD;       
            // create a new split (unless all nodes were requested)
            if (requestednodes < (size_t)m_numMPINodes)
            {
#ifdef USE2NDCOMM
                fprintf(stderr, "requestnodes: MPI_Comm_split %d\n", (node() < requestednodes) ? 1 : MPI_UNDEFINED); 
                fflush(stderr);
                MPI_Comm_split(communicator(), (node() < requestednodes) ? 1 : MPI_UNDEFINED, 0, &m_currentComm) || MpiFail("requestnodes: MPI_Comm_split");
                fprintf(stderr, "requestnodes: MPI_Comm_split -> %x\n", (int) m_currentComm); 
                fflush(stderr);
#endif
            }
            else
            {
                // leave m_currentComm as MPI_COMM_WORLD
                // and clip to #nodes
                requestednodes = m_numMPINodes;      
            }

            m_numNodesInUse = requestednodes;
            fprintf(stderr, "requestnodes [%s]: using %d out of %d MPI nodes (%d requested); we (%d) are %s\n",
                     msg, m_numNodesInUse, m_numMPINodes, (int) requestednodes,
                     CurrentNodeRank(), IsIdle() ? "out (idle)" : "in (participating)");
            fflush(stderr);
            Ping("requestnodes (after change)");
        }

        MPI_Comm Communicator() const { return m_currentComm; }
        size_t NumNodesInUse() const { return m_numNodesInUse; }
        size_t CurrentNodeRank() const { return m_myRank; }
        bool IsMainNode() const { return m_myRank == 0; }          // we are the chosen one--do extra stuff like saving the model to disk
        bool IsIdle() const { return CurrentNodeRank() >= NumNodesInUse(); }           // user had requested to not use this many nodes
        bool UsingAllNodes() const { return NumNodesInUse() == m_numMPINodes; }  // all nodes participate (used to check whether we can use MPI_Allreduce directly)
        size_t MainNodeRank() const {return 0;}

        // -----------------------------------------------------------------------
        // data-exchange functions (wrappers around MPI functions)
        // -----------------------------------------------------------------------

        // helpers to determine the MPI_Datatype of a pointer
        static MPI_Datatype GetDataType(char *)   { return MPI_CHAR; }
        static MPI_Datatype GetDataType(int *)    { return MPI_INT; }
        static MPI_Datatype GetDataType(float *)  { return MPI_FLOAT; }
        static MPI_Datatype GetDataType(double *) { return MPI_DOUBLE; }
        static MPI_Datatype GetDataType(size_t *) { return sizeof(size_t) == 4 ? MPI_UNSIGNED : MPI_LONG_LONG_INT; }

        // allreduce of a vector
        template<typename VECTORLIKEOBJECT>
        void AllReduce(VECTORLIKEOBJECT& accumulator) const
        {
            auto * dataptr = accumulator.data();
            size_t totalnumelements = accumulator.size();

            // use MPI to compute the sum over all elements in (dataptr, totalnumelements) and redistribute to all nodes
            if ((NumNodesInUse() > 1) && (Communicator() != MPI_COMM_NULL))
            {
                MPI_Allreduce(MPI_IN_PLACE, dataptr, (int)totalnumelements, GetDataType(dataptr), MPI_SUM, Communicator()) || MpiFail("allreduce: MPI_Allreduce");
            }
        }

        // wait for all ranks to reach here
        void WaitAll() 
        {
            MPI_Barrier(m_currentComm) || MpiFail("waitall: MPI_Barrier");
        }
    };
}}}
