#include <algorithm>
#include <fstream>
#include <iostream>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define DELIMITER "||##DELIMITATOR##||"
#define CLIENT_SENDING_OWNED_FILES 1
#define TRACKER_SENDING_BEGIN_DOWNLOAD 2
#define CLIENT_SENDING_SWARM_REQUEST 3
#define TRACKER_SENDING_SWARM 4
#define CLIENT_SENDING_FILE_INFO_REQUEST 5
#define TRACKER_SENDING_FILE_INFO 6
#define MARK_CLIENT_AS_PEER 7
#define PEER_REQUEST_SEGMENT 8
#define PEER_SENDING_SEGMENT 9
#define CLIENT_COMPLETED_FILE 10
#define CLIENT_COMPLETED_ALL 11
#define CLOSING 12

using namespace std;

enum fileType
{
    OWNED,
    WANTED
};

struct Segment
{
    string name;
    bool isOwned = false;
};

struct FileSwarm
{
    vector<int> peers;
    vector<int> seeders;
};

struct trackerFile
{
    vector<string> segments;
    FileSwarm fileSwarm;
};

struct trackerData
{
    unordered_map<string, trackerFile> files;
    bool everythingIsDone = false;
    int numberOfClientsTrackerReceivedOwnedFiles = 0;
    int numberOfFinishedClients = 0;
    int numberOfClients = 0;
};

struct clientFile
{
    fileType type;
    string filename;
    int numberOfSegments = 0;
    vector<Segment> segments;
    FileSwarm wantedFileSwarm;
    int nextPeer = 0;
};

struct clientSharedData
{
    vector<clientFile> *files;
    int clientRank;
    pthread_mutex_t *mutex;
    bool *uploadRunning;
};

// Function to extract the delimiters from the received messages
vector<string> removeDelimiter(const string &str)
{
    vector<string> substrings;
    string delimiter = DELIMITER;
    int posisitonStart = 0, posisitonEnd = 0;
    int delimiterLenght = delimiter.length();

    while ((size_t)(posisitonEnd = str.find(delimiter, posisitonStart)) != string::npos)
    {
        string substring = str.substr(posisitonStart, posisitonEnd - posisitonStart);
        substrings.push_back(substring);
        posisitonStart = posisitonEnd + delimiterLenght;
    }

    substrings.push_back(str.substr(posisitonStart));
    return substrings;
}

void *download_thread_func(void *arg)
{
    clientSharedData *clientData = (clientSharedData *)arg;
    vector<clientFile> *files = clientData->files;
    int rank = clientData->clientRank;
    pthread_mutex_t *mutex = clientData->mutex;

    for (auto &file : *files)
    {
        if (file.type == OWNED)
        {
            // Skip owned files
            continue;
        }

        int segmentsDownloaded = 0;

        for (auto &segment : file.segments)
        {
            // After the first segment is downloaded, send a message to the tracker to mark this client as a peer for that file
            if (segmentsDownloaded == 1)
            {
                MPI_Send(file.filename.c_str(), file.filename.size(), MPI_CHAR, TRACKER_RANK, MARK_CLIENT_AS_PEER, MPI_COMM_WORLD);
            }

            if (segment.isOwned)
            {
                // Skip already owned segments
                continue;
            }

            // Combine seeds and peers into a single vector
            vector<int> allSources = file.wantedFileSwarm.seeders;
            allSources.insert(allSources.end(), file.wantedFileSwarm.peers.begin(), file.wantedFileSwarm.peers.end());

            int numberOfSources = allSources.size();

            for (int i = 0; i < numberOfSources; i++)
            {

                int currentIndex = (file.nextPeer + i) % numberOfSources;
                int peerRank = allSources[currentIndex];

                // Build the swarm request message "filename||##DELIMITATOR##||segmentname" and send it to the peer
                string requestMessage = file.filename + DELIMITER + segment.name;
                MPI_Send(requestMessage.c_str(), requestMessage.size(), MPI_CHAR, peerRank, PEER_REQUEST_SEGMENT, MPI_COMM_WORLD);

                MPI_Status status;
                char responseBuffer[3] = {0}; // Buffer to store "OK" or "NO"
                MPI_Recv(responseBuffer, 2, MPI_CHAR, peerRank, PEER_SENDING_SEGMENT, MPI_COMM_WORLD, &status);
                string response(responseBuffer, 2);

                // Receive the response from the peer, if they have it, mark it as downloaded, otherwise try another peer
                if (response == "OK")
                {
                    pthread_mutex_lock(mutex);
                    segment.isOwned = true;
                    pthread_mutex_unlock(mutex);

                    // Update the index of the next peer for round robin
                    file.nextPeer = (currentIndex + 1) % numberOfSources;

                    segmentsDownloaded++;

                    // After downloading 10 segments, ask the tracker to update the swarm file
                    if (segmentsDownloaded % 10 == 0)
                    {
                        string swarmRequestMessage = file.filename;
                        MPI_Send(swarmRequestMessage.c_str(), swarmRequestMessage.size(), MPI_CHAR, TRACKER_RANK, CLIENT_SENDING_SWARM_REQUEST, MPI_COMM_WORLD);

                        MPI_Status swarmStatus;
                        MPI_Probe(TRACKER_RANK, TRACKER_SENDING_SWARM, MPI_COMM_WORLD, &swarmStatus);
                        int swarmLength;
                        MPI_Get_count(&swarmStatus, MPI_INT, &swarmLength);
                        vector<int> newSwarm(swarmLength);
                        MPI_Recv(newSwarm.data(), swarmLength, MPI_INT, TRACKER_RANK, TRACKER_SENDING_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                        // Update the swarm file
                        pthread_mutex_lock(mutex);
                        file.wantedFileSwarm.seeders.clear();
                        file.wantedFileSwarm.peers.clear();
                        int seedsNumber = newSwarm[0];
                        int peersNumber = newSwarm[1];
                        for (int seedsIndex = 0; seedsIndex < seedsNumber; seedsIndex++)
                        {
                            file.wantedFileSwarm.seeders.push_back(newSwarm[seedsIndex + 2]);
                        }
                        for (int peersIndex = 0; peersIndex < peersNumber; peersIndex++)
                        {
                            file.wantedFileSwarm.peers.push_back(newSwarm[peersIndex + 2 + seedsNumber]);
                        }
                        pthread_mutex_unlock(mutex);
                    }

                    // Move on to the next segment
                    break;
                }
                else
                {
                    // Attempt to get the segment from another peer
                    continue;
                }
            }
        }

        // Check if all segments have been downloaded
        bool hasAllSegments = true;
        for (auto &segment : file.segments)
        {
            if (!segment.isOwned)
            {
                hasAllSegments = false;
                break;
            }
        }

        // If all segments of the file are downloaded, create the output file and send a message to the tracker that the file is completely downloaded
        if (hasAllSegments)
        {
            string completionMessage = file.filename;
            MPI_Send(completionMessage.c_str(), completionMessage.size(), MPI_CHAR, TRACKER_RANK, CLIENT_COMPLETED_FILE, MPI_COMM_WORLD);

            string outputFile("client");
            outputFile += to_string(rank);
            outputFile += "_";
            outputFile += file.filename;

            ofstream outfile(outputFile);

            for (auto &segment : file.segments)
                outfile << segment.name << endl;

            outfile.close();
        }
    }

    // Finally, send a message to the tracker that the client has finished downloading all required files
    MPI_Send(NULL, 0, MPI_CHAR, TRACKER_RANK, CLIENT_COMPLETED_ALL, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    clientSharedData *clientData = (clientSharedData *)arg;
    vector<clientFile> *files = clientData->files;
    pthread_mutex_t *mutex = clientData->mutex;
    bool *running = clientData->uploadRunning;

    while (*running)
    {
        MPI_Status status;
        int flag;

        // Non-blocking check if there are requests for segments, so it can close properly at the end
        MPI_Iprobe(MPI_ANY_SOURCE, PEER_REQUEST_SEGMENT, MPI_COMM_WORLD, &flag, &status);
        if (flag)
        {
            int messageLength;
            MPI_Get_count(&status, MPI_CHAR, &messageLength);
            vector<char> buffer(messageLength + 1, 0);
            MPI_Recv(buffer.data(), messageLength, MPI_CHAR, status.MPI_SOURCE, PEER_REQUEST_SEGMENT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            string requestMessage(buffer.data(), messageLength);

            // From the message of the form "filename||##DELIMITATOR##||segmentname", extract the filename and the segment
            int delimiterPos = requestMessage.find(DELIMITER);
            string filename = requestMessage.substr(0, delimiterPos);
            string segmentHash = requestMessage.substr(delimiterPos + strlen(DELIMITER));

            // Check whether it has the requested segment or not
            bool hasSegment = false;
            pthread_mutex_lock(mutex);
            for (auto &file : *files)
            {
                if (file.filename == filename)
                {
                    for (auto &segment : file.segments)
                    {
                        if (segment.name == segmentHash && segment.isOwned)
                        {
                            hasSegment = true;
                            break;
                        }
                    }
                }
                if (hasSegment)
                {
                    break;
                }
            }
            pthread_mutex_unlock(mutex);

            // If it has the segment, send back "OK", otherwise send "NO"
            if (hasSegment)
            {
                string response = "OK";
                MPI_Send(response.c_str(), response.size(), MPI_CHAR, status.MPI_SOURCE, PEER_SENDING_SEGMENT, MPI_COMM_WORLD);
            }
            else
            {
                string response = "NO";
                MPI_Send(response.c_str(), response.size(), MPI_CHAR, status.MPI_SOURCE, PEER_SENDING_SEGMENT, MPI_COMM_WORLD);
            }
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank)
{
    trackerData data;

    data.numberOfClientsTrackerReceivedOwnedFiles = 0;
    data.numberOfFinishedClients = 0;
    data.numberOfClients = numtasks - 1;
    bool ok = true;
    MPI_Request requestsForSendingOKAfterInitialization[data.numberOfClients];

    while (!data.everythingIsDone)
    {
        MPI_Status status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int tag = status.MPI_TAG;
        int sourceOfMessage = status.MPI_SOURCE;
        int lenghtOfIncomingMessage;
        MPI_Get_count(&status, MPI_CHAR, &lenghtOfIncomingMessage);
        vector<char> messageBuffer(lenghtOfIncomingMessage);
        MPI_Recv(messageBuffer.data(), lenghtOfIncomingMessage, MPI_CHAR, sourceOfMessage, tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        string message(messageBuffer.data(), lenghtOfIncomingMessage);

        // Remove the delimiter from messages and split them into a list
        vector<string> splitMessage = removeDelimiter(message);
        string filename;
        int numberOfSegments, numberOfFiles;
        int splitMessageIndex = 0;
        vector<int> fileSwarmCombined;
        string fileInfoMessage;

        // Execute necessary actions based on the received tag
        switch (tag)
        {
        // If the client sends the files it owns, the tracker stores them in its database
        case CLIENT_SENDING_OWNED_FILES:
            data.numberOfClientsTrackerReceivedOwnedFiles++;

            numberOfFiles = atoi(splitMessage[splitMessageIndex++].c_str());
            for (int currentFile = 1; currentFile <= numberOfFiles; currentFile++)
            {
                filename = splitMessage[splitMessageIndex++];
                data.files[filename].segments.clear();
                numberOfSegments = atoi(splitMessage[splitMessageIndex++].c_str());
                for (int currentSegment = 1; currentSegment <= numberOfSegments; currentSegment++)
                {
                    data.files[filename].segments.push_back(splitMessage[splitMessageIndex++]);
                }
                data.files[filename].fileSwarm.seeders.push_back(sourceOfMessage);
            }

            // Send a non-blocking OK message to the client indicating it can start downloading
            MPI_Isend(&ok, 1, MPI_CXX_BOOL, sourceOfMessage, TRACKER_SENDING_BEGIN_DOWNLOAD, MPI_COMM_WORLD, requestsForSendingOKAfterInitialization + sourceOfMessage - 1);
            break;

        // Case when the client wants information about the file (number of segments and their names)
        case CLIENT_SENDING_FILE_INFO_REQUEST:
            filename = splitMessage[splitMessageIndex];
            fileInfoMessage.clear();
            fileInfoMessage += to_string(data.files[filename].segments.size()) + DELIMITER;
            for (auto segment : data.files[filename].segments)
            {
                fileInfoMessage += segment + DELIMITER;
            }
            MPI_Send(fileInfoMessage.c_str(), fileInfoMessage.size(), MPI_CHAR, sourceOfMessage, TRACKER_SENDING_FILE_INFO, MPI_COMM_WORLD);
            break;

        // Case when the client requests the swarm of a file for the first time or wants to update it
        case CLIENT_SENDING_SWARM_REQUEST:
            filename = splitMessage[splitMessageIndex];

            fileSwarmCombined.push_back(data.files[filename].fileSwarm.seeders.size());
            fileSwarmCombined.push_back(data.files[filename].fileSwarm.peers.size());

            fileSwarmCombined.insert(fileSwarmCombined.end(), data.files[filename].fileSwarm.seeders.begin(), data.files[filename].fileSwarm.seeders.end());

            fileSwarmCombined.insert(fileSwarmCombined.end(), data.files[filename].fileSwarm.peers.begin(), data.files[filename].fileSwarm.peers.end());

            MPI_Send(fileSwarmCombined.data(), fileSwarmCombined.size(), MPI_INT, sourceOfMessage, TRACKER_SENDING_SWARM, MPI_COMM_WORLD);
            break;

        // Case when the client downloads the first segment of a file and can be marked as a peer for that file
        case MARK_CLIENT_AS_PEER:
            filename = splitMessage[splitMessageIndex];
            data.files[filename].fileSwarm.peers.push_back(sourceOfMessage);
            break;

        // Case when a client has finished downloading a file
        case CLIENT_COMPLETED_FILE:
            filename = splitMessage[splitMessageIndex];
            data.files[filename].fileSwarm.seeders.push_back(sourceOfMessage);
            for (auto iterator = data.files[filename].fileSwarm.peers.begin(); iterator != data.files[filename].fileSwarm.peers.end(); iterator++)
            {
                if (*iterator == sourceOfMessage)
                {
                    data.files[filename].fileSwarm.peers.erase(iterator);
                    break;
                }
            }
            break;

        // Case when the client has finished downloading all requested files
        case CLIENT_COMPLETED_ALL:
            data.numberOfFinishedClients++;
            break;
        }

        // Wait for all clients to receive OK to start downloading, then wait all processes at the barrier
        if (data.numberOfClientsTrackerReceivedOwnedFiles == data.numberOfClients)
        {
            MPI_Waitall(data.numberOfClients, requestsForSendingOKAfterInitialization, MPI_STATUS_IGNORE);
            MPI_Barrier(MPI_COMM_WORLD);
            data.numberOfClientsTrackerReceivedOwnedFiles = -1;
        }

        // If all clients have finished, close the tracker's running loop and send the closing signal to clients
        if (data.numberOfFinishedClients == data.numberOfClients)
        {
            for (int client = 1; client <= data.numberOfClients; client++)
            {
                MPI_Send(NULL, 0, MPI_CHAR, client, CLOSING, MPI_COMM_WORLD);
            }
            data.everythingIsDone = true;
        }
    }
}

void peer(int numtasks, int rank)
{
    vector<clientFile> files;

    // Open the input file and read the input data
    ifstream input(("in" + to_string(rank) + ".txt").c_str());
    int numberOfFilesPeerOwns, numberOfFilesPeerWants;
    input >> numberOfFilesPeerOwns;
    files.resize(numberOfFilesPeerOwns);
    for (auto &file : files)
    {
        input >> file.filename;
        file.type = OWNED;
        input >> file.numberOfSegments;
        file.segments.resize(file.numberOfSegments);
        for (auto &segment : file.segments)
        {
            input >> segment.name;
            segment.isOwned = true;
        }
    }
    input >> numberOfFilesPeerWants;
    for (int currentFile = 0; currentFile < numberOfFilesPeerWants; currentFile++)
    {
        clientFile file;
        input >> file.filename;
        file.type = WANTED;
        files.push_back(file);
    }

    // Send the owned files to the tracker
    // A single message will be sent using delimiters.
    // It will look like this: "numberOfFilesPeerOwn||##DELIMITATOR##||filename1||##DELIMITATOR##||numberOfSegmentsFilename1||##DELIMITATOR##||
    // Segment1File1||##DELIMITATOR##||SegmentIfileIfile1||##DELIMITATOR##||etc"
    string messageToSendToTracker = "";
    messageToSendToTracker = to_string(numberOfFilesPeerOwns) + DELIMITER;
    for (int currentOwnedFile = 0; currentOwnedFile < numberOfFilesPeerOwns; currentOwnedFile++)
    {
        clientFile file = files[currentOwnedFile];
        messageToSendToTracker += file.filename + DELIMITER;
        messageToSendToTracker += to_string(file.numberOfSegments) + DELIMITER;
        for (auto &segment : file.segments)
            messageToSendToTracker += segment.name + DELIMITER;
    }

    MPI_Send(messageToSendToTracker.c_str(), messageToSendToTracker.size(), MPI_CHAR, TRACKER_RANK, CLIENT_SENDING_OWNED_FILES, MPI_COMM_WORLD);

    bool startDownload = false;
    MPI_Recv(&startDownload, 1, MPI_CXX_BOOL, TRACKER_RANK, TRACKER_SENDING_BEGIN_DOWNLOAD, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // Clients wait at the barrier so that all of them start downloading simultaneously
    MPI_Barrier(MPI_COMM_WORLD);

    MPI_Status statusMPI, fileInfoResponseStatus;
    MPI_Request fileInfoRequest;
    int fileSwarmResponseLenght = 0, fileInfoResponseLenght = 0;
    vector<int> fileSwarmCombined;
    string fileInfoResponse;

    for (int fileIndex = numberOfFilesPeerOwns; (size_t)fileIndex < files.size(); fileIndex++)
    {
        string filename = files[fileIndex].filename;
        MPI_Send(filename.c_str(), filename.size(), MPI_CHAR, TRACKER_RANK, CLIENT_SENDING_FILE_INFO_REQUEST, MPI_COMM_WORLD);

        MPI_Probe(TRACKER_RANK, TRACKER_SENDING_FILE_INFO, MPI_COMM_WORLD, &statusMPI);
        MPI_Get_count(&statusMPI, MPI_CHAR, &fileInfoResponseLenght);
        vector<char> messageBuffer(fileInfoResponseLenght);
        MPI_Irecv(messageBuffer.data(), fileInfoResponseLenght, MPI_CHAR, TRACKER_RANK, TRACKER_SENDING_FILE_INFO, MPI_COMM_WORLD, &fileInfoRequest);

        // While waiting for fileInfo, request and process fileSwarm
        MPI_Send(filename.c_str(), filename.size(), MPI_CHAR, TRACKER_RANK, CLIENT_SENDING_SWARM_REQUEST, MPI_COMM_WORLD);
        MPI_Probe(TRACKER_RANK, TRACKER_SENDING_SWARM, MPI_COMM_WORLD, &statusMPI);
        MPI_Get_count(&statusMPI, MPI_INT, &fileSwarmResponseLenght);
        vector<int> fileSwarmCombined(fileSwarmResponseLenght);
        MPI_Recv(fileSwarmCombined.data(), fileSwarmResponseLenght, MPI_INT, TRACKER_RANK, TRACKER_SENDING_SWARM, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        int seedsNumber = fileSwarmCombined[0];
        int peersNumber = fileSwarmCombined[1];
        for (int seedsIndex = 0; seedsIndex < seedsNumber; seedsIndex++)
        {
            files[fileIndex].wantedFileSwarm.seeders.push_back(fileSwarmCombined[seedsIndex + 2]);
        }
        for (int peersIndex = 0; peersIndex < peersNumber; peersIndex++)
        {
            files[fileIndex].wantedFileSwarm.peers.push_back(fileSwarmCombined[peersIndex + 2 + seedsNumber]);
        }
        fileSwarmCombined.clear();

        // Process the fileInfo received from the tracker
        MPI_Wait(&fileInfoRequest, &fileInfoResponseStatus);
        string fileInfoResponse(messageBuffer.data(), fileInfoResponseLenght);
        vector<string> splitMessage = removeDelimiter(fileInfoResponse);
        int splitMessageIndex = 0;

        files[fileIndex].numberOfSegments = atoi(splitMessage[splitMessageIndex++].c_str());
        for (int segmentIndex = 0; segmentIndex < files[fileIndex].numberOfSegments; segmentIndex++)
        {
            Segment segment;
            segment.name = splitMessage[splitMessageIndex++];
            segment.isOwned = false;
            files[fileIndex].segments.push_back(segment);
        }
    }

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    bool uploadRunning = true;
    clientSharedData clientData{&files, rank, &mutex, &uploadRunning};
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&clientData);
    if (r)
    {
        printf("Error creating download thread\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&clientData);
    if (r)
    {
        printf("Error creating upload thread\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r)
    {
        printf("Error waiting for download thread\n");
        exit(-1);
    }

    MPI_Recv(NULL, 0, MPI_CHAR, TRACKER_RANK, CLOSING, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    uploadRunning = false;

    r = pthread_join(upload_thread, &status);
    if (r)
    {
        printf("Error waiting for upload thread\n");
        exit(-1);
    }

    pthread_mutex_destroy(&mutex);
}

int main(int argc, char *argv[])
{
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE)
    {
        fprintf(stderr, "MPI does not have support for multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK)
    {
        tracker(numtasks, rank);
    }
    else
    {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
