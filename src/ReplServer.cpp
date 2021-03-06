#include <iostream>
#include <exception>
#include "ReplServer.h"

const time_t secs_between_repl = 20;
const unsigned int max_servers = 10;

/*********************************************************************************************
 * ReplServer (constructor) - creates our ReplServer. Initializes:
 *
 *    verbosity - passes this value into QueueMgr and local, plus each connection
 *    _time_mult - how fast to run the simulation - 2.0 = 2x faster
 *    ip_addr - which ip address to bind the server to
 *    port - bind the server here
 *
 *********************************************************************************************/
ReplServer::ReplServer(DronePlotDB &plotdb, float time_mult)
                              :_queue(1),
                               _plotdb(plotdb),
                               _shutdown(false), 
                               _time_mult(time_mult),
                               _verbosity(1),
                               _ip_addr("127.0.0.1"),
                               _port(9999)
{
}

ReplServer::ReplServer(DronePlotDB &plotdb, const char *ip_addr, unsigned short port, float time_mult,
                                          unsigned int verbosity)
                                 :_queue(verbosity),
                                  _plotdb(plotdb),
                                  _shutdown(false), 
                                  _time_mult(time_mult), 
                                  _verbosity(verbosity),
                                  _ip_addr(ip_addr),
                                  _port(port)

{
}

ReplServer::~ReplServer() {

}


/**********************************************************************************************
 * getAdjustedTime - gets the time since the replication server started up in seconds, modified
 *                   by _time_mult to speed up or slow down
 **********************************************************************************************/

time_t ReplServer::getAdjustedTime() {
   return static_cast<time_t>((time(NULL) - _start_time) * _time_mult);
}

/**********************************************************************************************
 * replicate - the main function managing replication activities. Manages the QueueMgr and reads
 *             from the queue, deconflicting entries and populating the DronePlotDB object with
 *             replicated plot points.
 *
 *    Params:  ip_addr - the local IP address to bind the listening socket
 *             port - the port to bind the listening socket
 *             
 *    Throws: socket_error for recoverable errors, runtime_error for unrecoverable types
 **********************************************************************************************/

void ReplServer::replicate(const char *ip_addr, unsigned short port) {
   _ip_addr = ip_addr;
   _port = port;
   replicate();
}

void ReplServer::replicate() {

   // Track when we started the server
   _start_time = time(NULL);
   _last_repl = 0;

   // Set up our queue's listening socket
   _queue.bindSvr(_ip_addr.c_str(), _port);
   _queue.listenSvr();

   if (_verbosity >= 2)
      std::cout << "Server bound to " << _ip_addr << ", port: " << _port << " and listening\n";

  
   // Replicate until we get the shutdown signal
   while (!_shutdown) {

      // Check for new connections, process existing connections, and populate the queue as applicable
      _queue.handleQueue();     

      // See if it's time to replicate and, if so, go through the database, identifying new plots
      // that have not been replicated yet and adding them to the queue for replication
      if (getAdjustedTime() - _last_repl > secs_between_repl) {

         queueNewPlots();
         _last_repl = getAdjustedTime();
      }
      
      // Check the queue for updates and pop them until the queue is empty. The pop command only returns
      // incoming replication information--outgoing replication in the queue gets turned into a TCPConn
      // object and automatically removed from the queue by pop
      std::string sid;
      std::vector<uint8_t> data;
      while (_queue.pop(sid, data)) {

         // Incoming replication--add it to this server's local database
         addReplDronePlots(data);         
      }       

      usleep(1000);
   }
   //Check the DB one last time for inconsistencies
   removeDuplicates();
   updateSkewDB();
   std::cout << "Shutting down replication";
}

/**********************************************************************************************
 * queueNewPlots - looks at the database and grabs the new plots, marshalling them and
 *                 sending them to the queue manager
 *
 *    Returns: number of new plots sent to the QueueMgr
 *
 *    Throws: socket_error for recoverable errors, runtime_error for unrecoverable types
 **********************************************************************************************/

unsigned int ReplServer::queueNewPlots() {
   std::vector<uint8_t> marshall_data;
   unsigned int count = 0;

   if (_verbosity >= 3)
      std::cout << "Replicating plots.\n";

   // Loop through the drone plots, looking for new ones
   std::list<DronePlot>::iterator dpit = _plotdb.begin();
   for ( ; dpit != _plotdb.end(); dpit++) {

      // If this is a new one, marshall it and clear the flag
      if (dpit->isFlagSet(DBFLAG_NEW)) {
         
         dpit->serialize(marshall_data);
         dpit->clrFlags(DBFLAG_NEW);

         count++;
      }
      if (marshall_data.size() % DronePlot::getDataSize() != 0)
         throw std::runtime_error("Issue with marshalling!");

   }
  
   if (count == 0) {
      if (_verbosity >= 3)
         std::cout << "No new plots found to replicate.\n";

      return 0;
   }
 
   // Add the count onto the front
   if (_verbosity >= 3)
      std::cout << "Adding in count: " << count << "\n";

   uint8_t *ctptr_begin = (uint8_t *) &count;
   marshall_data.insert(marshall_data.begin(), ctptr_begin, ctptr_begin+sizeof(unsigned int));

   // Send to the queue manager
   if (marshall_data.size() > 0) {
      _queue.sendToAll(marshall_data);
   }

   if (_verbosity >= 2) 
      std::cout << "Queued up " << count << " plots to be replicated.\n";

   //Functions to ensure DB consistency
   removeDuplicates();
   updateSkewDB();

   return count;
}

/**********************************************************************************************
 * addReplDronePlots - Adds drone plots to the database from data that was replicated in. 
 *                     Deconflicts issues between plot points.
 * 
 * Params:  data - should start with the number of data points in a 32 bit unsigned integer, 
 *                 then a series of drone plot points
 *
 **********************************************************************************************/

void ReplServer::addReplDronePlots(std::vector<uint8_t> &data) {
   if (data.size() < 4) {
      throw std::runtime_error("Not enough data passed into addReplDronePlots");
   }

   if ((data.size() - 4) % DronePlot::getDataSize() != 0) {
      throw std::runtime_error("Data passed into addReplDronePlots was not the right multiple of DronePlot size");
   }

   // Get the number of plot points
   unsigned int *numptr = (unsigned int *) data.data();
   unsigned int count = *numptr;

   // Store sub-vectors for efficiency
   std::vector<uint8_t> plot;
   auto dptr = data.begin() + sizeof(unsigned int);

   for (unsigned int i=0; i<count; i++) {
      plot.clear();
      plot.assign(dptr, dptr + DronePlot::getDataSize());
      addSingleDronePlot(plot);
      dptr += DronePlot::getDataSize();      
   }

   if (_verbosity >= 2)
      std::cout << "Replicated in " << count << " plots\n";   
}


/**********************************************************************************************
 * addSingleDronePlot - Takes in binary serialized drone data and adds it to the database. 
 *
 **********************************************************************************************/

void ReplServer::addSingleDronePlot(std::vector<uint8_t> &data) {
   DronePlot tmp_plot;

   tmp_plot.deserialize(data);

   //If new node is seen that has lower nodeID (default 1)
   if (tmp_plot.node_id < masterNode) { masterNode = tmp_plot.node_id; }

   //Need to check for duplicate points before adding
   for (auto & element : _plotdb) {
      if (element.latitude == tmp_plot.latitude && element.longitude == tmp_plot.longitude && element.drone_id == tmp_plot.drone_id) {
         //Compare times
         auto timeDif = element.timestamp - tmp_plot.timestamp;
         //Duplicate Plots, update time differential
         if (abs(timeDif) < 15.0) {
            if (tmp_plot.node_id == masterNode) {
               updateOffset(element.node_id, timeDif);
            }
            return;
         }
         //Same location but too large an offset to be duplicate
         else {
               _plotdb.addPlot(tmp_plot.drone_id, tmp_plot.node_id, tmp_plot.timestamp + getOffset(tmp_plot.node_id), tmp_plot.latitude, tmp_plot.longitude);
               tmp_plot.setFlags(DBFLAG_SYNCD);
         }
      }
   }

   //Add the replication point if it isn't a possible duplicate
   _plotdb.addPlot(tmp_plot.drone_id, tmp_plot.node_id, tmp_plot.timestamp + getOffset(tmp_plot.node_id), tmp_plot.latitude, tmp_plot.longitude);
   tmp_plot.setFlags(DBFLAG_SYNCD);
}

//Function that will iterate over all items in the database and remove duplicate entries
void ReplServer::removeDuplicates() {
   auto outer = _plotdb.begin();
   while (outer != _plotdb.end()) {
      auto inner = _plotdb.begin();
      while (inner != _plotdb.end()) {
         if (outer->latitude == inner->latitude && outer->longitude == inner->longitude && outer->drone_id == inner->drone_id && outer->node_id != inner->node_id) {
            auto timeDif = outer->timestamp - inner->timestamp;
            if (abs(timeDif) < 10.0) {
               if (inner->node_id == masterNode) {
                  auto skew = inner->timestamp - outer->timestamp;
                  updateOffset(outer->node_id, skew);
               }
               else if (outer->node_id == masterNode) {
                  auto skew = outer->timestamp - inner->timestamp;
                  updateOffset(inner->node_id, skew);
               }
               inner = _plotdb.erase(inner);
            }
            else { inner++; }
         }
         else { inner++; }
      }
      outer++;
   }
}


void ReplServer::shutdown() {
   _shutdown = true;
}

//Returns the stored skew between master and given nodeID
int ReplServer::getOffset(int nodeID) {
   if (timeDiffs[nodeID]){ return timeDiffs[nodeID]; }
   return 0;
}


//Updates the time skew between the chosen master and given nodeID
void ReplServer::updateOffset(int nodeID, long skew) {
   std::cout << "Comparing " << timeDiffs[nodeID] << " vs " << skew << "\n";
   if (timeDiffs[nodeID] != skew) {
      timeDiffs[nodeID] = skew;
      std::cout << "Updating offset between master and " << nodeID << " to " << skew << "\n";
   }
}

void ReplServer::updateSkewDB() {
   //Iterate over plotDB
   for (auto & plot : _plotdb) {
      //See if plot hasn't been synced
      if (!plot.isFlagSet(DBFLAG_SYNCD)) {
         //We have a non-zero skew value
         if (getOffset(plot.node_id) != 0) {
            //Modify time-stamp by known skew and set sync'd flag
            plot.timestamp = plot.timestamp + getOffset(plot.node_id);
            plot.setFlags(DBFLAG_SYNCD);
         }
      }
   }
}

