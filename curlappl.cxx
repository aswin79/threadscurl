nf-3070603]$ vi test.1.cpp



  #include <stdlib.h>
  #include <sqlite3.h>
  #include <future>
  #include <thread>
  #include <chrono>
  #include <iostream>
  #include <mutex>
  #include <vector>
  #include <queue>
  #include<boost/tokenizer.hpp>
  #include <string>
  #include <signal.h>
  #include <unistd.h>
  #include <cstring>
  #include <curl/curl.h>
  typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
  using namespace std;


  /*
     main class
     implements queing to monitor sql data entries
     keeps sql handle open throughout
     keeps querying targets periodically

  */

  class grabberWriter {
  public:
        grabberWriter(string  dbName,string instAdd, int numGr, int maxFailures);
        int addQueue (string target);
        int callback( int argc, char **argv, char **azColName);
        static int curlWriter(char *data, size_t size, size_t nmemb, std::string *buffer);
        int insertDB();
        void closeDB();
        void run();
        void execCurl(int id);
        void execCurl2(int id);

  private:
        mutex _queueMutex;
        string _instanceAddress;
        queue<string> _dbEntries;
        condition_variable _inUse;
        int _maxNumFailures=30;
        int _numInstances=0;
        sqlite3 *_db;
  };

  // db to open
  // base address of instance to monitor
  // number of instances to monitor
  grabberWriter::grabberWriter(string dbName, string instAdd, int numGr, int maxFailures) {
     int dbRet = sqlite3_open(dbName.c_str(), &_db);
     if( dbRet ) {
        cerr<< "Can't open database: "<< sqlite3_errmsg(_db)<<". Exiting."<<endl;
        exit(-1);
     }
     _instanceAddress =instAdd;
     _numInstances = numGr;
     _maxNumFailures = maxFailures;
     cout<< "Opened database successfully numInstances:"<<_numInstances<<endl;
  }


  /* add records to queue
    mutex needed to make sure queue isnt modified while being added.
   */
  int grabberWriter::addQueue (string target) {
     cout<<" addQueueaddQueue Called "<<target<<endl;
     unique_lock<mutex>  l(_queueMutex);
     _dbEntries.push(target);
     l.unlock();
     _inUse.notify_one(); // notify about _inUse after releasing lock so that db can be updated with newly added records.
     return 0;
  }


  /* results from curl need to be post processed to extract bytes transferred etc.
     This copies the results from curl to the string buffer
  */
  int  grabberWriter::curlWriter(char *data, size_t size, size_t nmemb, string *buffer) {
        int result = 0;
        if (buffer != NULL) {
          buffer->append(data, size * nmemb);
          result = size * nmemb;
        }
        return result;
  }

  /* not used actually since we are only writing into the sql db.
  */
  int grabberWriter::callback( int argc, char **argv, char **azColName) {
     int i;
     for(i = 0; i<argc; i++) {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
     }
     printf("\n");
     return 0;
  }

  /*
        to make the sqllite routine use the object method.
  */
  static int c_callback(void *param, int argc, char **argv, char **azColName)
  {
      grabberWriter* g = reinterpret_cast<grabberWriter*>(param);
      return g->callback(argc, argv, azColName);
  }


  /*
      monitor record queues and insert whenever the grabber produces one.
   */
  int grabberWriter::insertDB(  ) {

     char *zErrMsg = 0;
     int rc=0;
     unique_lock<mutex>  l(_queueMutex);
     while (1) {
            while ( _dbEntries.empty()) {
                cout<<" waiting for queues in insertDB "<<endl;
                _inUse.wait(l);// no record to insert yet...so give up lock and wait for _inUse.notify()
            }


        while (!_dbEntries.empty()) {
              string name = _dbEntries.front();
              _dbEntries.pop();
              l.unlock(); // give up lock to monitor to push into queue while we access sql db to insert.
              int rc = sqlite3_exec(_db, name.c_str(), c_callback, 0, &zErrMsg);
              cout<<" executing "<<name <<endl;
              if( rc !=  SQLITE_OK ){
                cerr<< "SQL error: "<< zErrMsg<<endl;
                sqlite3_free(zErrMsg);
             } else {
               cout<<"Records created successfully!."<<endl;
             }
         //get lock to access queue
         l.lock();
      }
    }
    return 1;
  }

  /* close records */
  void grabberWriter::closeDB() {
     sqlite3_close(_db);
     cout<<"Records closed "<<endl;
  }


  vector <grabberWriter*> GWVec;

  // close all dbs on exit.
  // signal handler to monitor CTRL+C type signals used to denote program conclusion
  void my_handler(int s){
    for (auto & X : GWVec)
      X->closeDB();
     exit(1);

  }



  //  there is a limit on opened file descriptors so popen might have its limitations,
  //  no need to install boost/ ms rest libraries which can provide the same functionality
  // more threadsafe since the various callbacks in ncurl has not been fully tested
  void grabberWriter::execCurl(int id) {
      std::array<char, 128> buffer;
      std::string result;
      string cmd = string("curl  --anyauth -m 5 ")+_instanceAddress+to_string(id)+string("/usage");

      while (1) {

        auto pipe = popen(cmd.c_str(), "r");
        if (!pipe) throw std::runtime_error("popen() failed!");

        while (!feof(pipe))
        {
          if (fgets(buffer.data(), 128, pipe) != nullptr)
              result += buffer.data();
        }

        auto rc = pclose(pipe);

        if (rc == EXIT_SUCCESS)
        {
          std::cout <<cmd<< " SUCCESS\n";
          string sql1 = string("INSERT INTO emp(id,name) values (")+to_string(id)+string(",'JOE'); ");
          addQueue(sql1.c_str());
        }
        else
        {
            std::cout <<cmd << "FAILED\n";
        }
         std::this_thread::sleep_for (std::chrono::seconds(5));
    }

  }

  // better performance sinces named pipes can be slower
  void grabberWriter::execCurl2(int id) {
    cout<<" grabberWriter::execCurl2 "<<id<<endl;
    array<char, 128> buffer;
    string result;
    string cmd = _instanceAddress+to_string(id)+string("/usage");

    CURL *curl;
    curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT ,5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(curl, CURLOPT_URL, cmd.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  &grabberWriter::curlWriter);
    string data;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,&data);

    CURLcode  res;
    char errbuf[CURL_ERROR_SIZE];
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    boost::char_separator<char> sep{","};
    long cnt=0,lastDataTrf=0,dataTrf=0, lid=0,timeStamp=0;
    int numFailures=0;
    while (numFailures<_maxNumFailures) {
         data="";
         res= curl_easy_perform(curl);
         // did not respond in a reasonable amount of time retry.
         if(res != CURLE_OK) {
            cout<< " Error "<<res<<" "<<errbuf<<" "<<curl_easy_strerror(res)<<endl;
            ++numFailures;
         }
         else {
          numFailures=0;
          std::cout <<cmd<< " SUCCESS\n";
          cout<<cmd<<" : "<<data<<endl;
          tokenizer tok(data,sep);
          int cnttok=0;
          for(auto &t : tok) {
            cout << t << "\n";
            if(cnttok== 0)
                     timeStamp=stol(t);
            if(cnttok== 1)
                     dataTrf=stol(t);
            cnttok++;
          }
          if(cnt>0) {
               lid = dataTrf - lastDataTrf;
          }
          cnt++;
          lastDataTrf = dataTrf;
          string sql1 = string("INSERT INTO  usage_data(node_id ,timestamp ,kb ) values (")+to_string(id)+string(",")+to_string(timeStamp)+string(",")+to_string(lid);
          addQueue(sql1.c_str());
         }
         std::this_thread::sleep_for (std::chrono::seconds(5));
    }
    cerr<<" Num of timeouts exceeded "<<numFailures<<". Will no longer monitor"<<cmd<<endl;
    curl_easy_cleanup(curl);
  }

  void  grabberWriter::run() {

     // Launch a thread to monitor records to insert into sql db */
     auto futureDBWriter = std::async(std::launch::async, &grabberWriter::insertDB, this);

     // Launch threads to query the target every 5 seconds and create records to insert
    vector< future<void>> grabber_monitor_futures;
     for (int i=0;i<_numInstances;i++) {
       grabber_monitor_futures.push_back(std::async(std::launch::async, &grabberWriter::execCurl2,  this, i+1));
     }
     // wait until exit....this will wait forever since exit is via CTRL+C
     auto result = futureDBWriter.get(); // Get result.
  }


  int main(int argc, char* argv[]) {

     int numGr =0;
     // need to initialize these 2 appropriately.
     string dbName="";
     string ipAddr="";
     if (argc > 1) {
      // should return 0 if argv not an integer
      numGr =atoi(argv[1]);
     }
     if(numGr < 1) {
         cerr<<" Needs valid (>0) number of instances to monitor. Exiting."<<endl;
         exit(-1);
    }
     /* Open sql database */
     grabberWriter* gW= new grabberWriter(dbName.c_str(), ipAddr.c_str(), numGr, 100);
     GWVec.push_back(gW);

    /* program will run until CTRL+C */
     struct sigaction sigIntHandler;
     sigIntHandler.sa_handler = my_handler;
     sigemptyset(&sigIntHandler.sa_mask);
     sigIntHandler.sa_flags = 0;
     sigaction(SIGINT, &sigIntHandler, NULL);
     curl_global_init(CURL_GLOBAL_ALL);

     /* RUN !*/
     gW->run();
     return 0;
  }


