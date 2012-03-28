/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvAccessCPP is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
 
#define __STDC_LIMIT_MACROS 1
#include <pv/blockingTCP.h>
#include <pv/inetAddressUtil.h>
#include <pv/caConstants.h>
#include <pv/logger.h>
#include <pv/hexDump.h>
#include <pv/likely.h>

#include <pv/lock.h>
#include <pv/byteBuffer.h>
#include <pv/epicsException.h>
#include <pv/noDefaultMethods.h>
#include <pv/CDRMonitor.h>

#include <osdSock.h>
#include <osiSock.h>
#include <epicsThread.h>

#include <sys/types.h>
#include <algorithm>
#include <sstream>


#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

using namespace epics::pvData;

using std::max;
using std::min;
using std::ostringstream;

namespace epics {
namespace pvAccess {

        /*
        class MonitorSender : public TransportSender, public NoDefaultMethods {
        public:
            MonitorSender(Mutex* monitorMutex, GrowingCircularBuffer<
                    TransportSender*>* monitorSendQueue) :
                _monitorMutex(monitorMutex),
                        _monitorSendQueue(monitorSendQueue) {
            }

            virtual ~MonitorSender() {
            }

            virtual void lock() {
            }

            virtual void unlock() {
            }

            virtual void acquire() {
            }

            virtual void release() {
            }

            virtual void
            send(ByteBuffer* buffer, TransportSendControl* control);

        private:
            Mutex* _monitorMutex;
            GrowingCircularBuffer<TransportSender*>* _monitorSendQueue;
        };
        */

        PVDATA_REFCOUNT_MONITOR_DEFINE(blockingTCPTransport);

        const double BlockingTCPTransport::_delay = 0.01;

        BlockingTCPTransport::BlockingTCPTransport(Context::shared_pointer const & context,
                SOCKET channel, std::auto_ptr<ResponseHandler>& responseHandler,
                int receiveBufferSize, int16 priority) :
                    _channel(channel),
                    _priority(priority),
                    _responseHandler(responseHandler),
                    _markerPeriodBytes(MARKER_PERIOD),
                    _flushStrategy(DELAYED),
                    _rcvThreadId(0),
                    _sendThreadId(0),
                    //_monitorSender(new MonitorSender(&_monitorMutex,_monitorSendQueue)),
                    _context(context),
                    _autoDelete(true),
                    _remoteTransportRevision(0),
                    _remoteTransportReceiveBufferSize(MAX_TCP_RECV),
                    _remoteTransportSocketReceiveBufferSize(MAX_TCP_RECV),
                    _sendQueue(),
                    //_monitorSendQueue(),
                    _nextMarkerPosition(_markerPeriodBytes),
                    _sendPending(false),
                    _lastMessageStartPosition(0),
                    _lastSegmentedMessageType(0),
                    _lastSegmentedMessageCommand(0),
                    _flushRequested(false),
                    _sendBufferSentPosition(0),
                    _byteOrderFlag((EPICS_BYTE_ORDER == EPICS_ENDIAN_BIG) ? 0x80 : 0x00),
                    _storedPayloadSize(0),
                    _storedPosition(0),
                    _storedLimit(0),
                    _version(0),
                    _packetType(0),
                    _command(0),
                    _payloadSize(0),
                    _stage(READ_FROM_SOCKET),
                    _totalBytesReceived(0),
                    _closed(),
                    _sendThreadExited(false),
                    _verified(false),
                    _markerToSend(0),
                    _totalBytesSent(0),
                    _remoteBufferFreeSpace(INT64_MAX)
        {
            PVDATA_REFCOUNT_MONITOR_CONSTRUCT(blockingTCPTransport);

            // TODO minor tweak: deque size is not preallocated...
            
            unsigned int bufferSize = max((int)(MAX_TCP_RECV+MAX_ENSURE_DATA_BUFFER_SIZE), receiveBufferSize);
            // size must be "aligned"
            bufferSize = (bufferSize + (CA_ALIGNMENT - 1)) & (~(CA_ALIGNMENT - 1));
            
            _socketBuffer = new ByteBuffer(bufferSize);
            _socketBuffer->setPosition(_socketBuffer->getLimit());
            _startPosition = _socketBuffer->getPosition();

            // allocate buffer
            _sendBuffer = new ByteBuffer(bufferSize);
            _maxPayloadSize = _sendBuffer->getSize() - 2*CA_MESSAGE_HEADER_SIZE; // one for header, one for flow control

            // get TCP send buffer size
            osiSocklen_t intLen = sizeof(int);
            int retval = getsockopt(_channel, SOL_SOCKET, SO_SNDBUF, (char *)&_socketSendBufferSize, &intLen);
            if(unlikely(retval<0)) {
                _socketSendBufferSize = MAX_TCP_RECV;
                char errStr[64];
                epicsSocketConvertErrnoToString(errStr, sizeof(errStr));
                LOG(logLevelDebug,
                        "Unable to retrieve socket send buffer size: %s",
                        errStr);
            }

            // get remote address
            osiSocklen_t saSize = sizeof(sockaddr);
            retval = getpeername(_channel, &(_socketAddress.sa), &saSize);
            if(unlikely(retval<0)) {
                char errStr[64];
                epicsSocketConvertErrnoToString(errStr, sizeof(errStr));
                LOG(logLevelError,
                        "Error fetching socket remote address: %s",
                        errStr);
            }

            // set receive timeout so that we do not have problems at shutdown (recvfrom would block)
            struct timeval timeout;
            memset(&timeout, 0, sizeof(struct timeval));
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            if (unlikely(::setsockopt (_channel, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0))
            {
                char errStr[64];
                epicsSocketConvertErrnoToString(errStr, sizeof(errStr));
                LOG(logLevelError,
                    "Failed to set SO_RCVTIMEO for TDP socket %s: %s.",
                    inetAddressToString(_socketAddress).c_str(), errStr);
            }
            
            // TODO this will create marker with invalid endian flag
            // prepare buffer
            clearAndReleaseBuffer();
        }

        BlockingTCPTransport::~BlockingTCPTransport() {
            PVDATA_REFCOUNT_MONITOR_DESTRUCT(blockingTCPTransport);

            close(true);

            // TODO use auto_ptr class members
            
            delete _socketBuffer;
            delete _sendBuffer;
        }

        // TODO consider epics::pvData::Thread
        void BlockingTCPTransport::start() {
            
            // TODO this was in constructor
            // add to registry
            Transport::shared_pointer thisSharedPtr = shared_from_this();
            _context->getTransportRegistry()->put(thisSharedPtr);

            
            String socketAddressString = inetAddressToString(_socketAddress);

            //
            // start receive thread
            //
            
            String threadName = "TCP-receive " + socketAddressString;
            LOG(logLevelDebug, "Starting thread: %s", threadName.c_str());

            _rcvThreadId = epicsThreadCreate(threadName.c_str(),
                    epicsThreadPriorityMedium,
                    epicsThreadGetStackSize(epicsThreadStackMedium),
                    BlockingTCPTransport::rcvThreadRunner, this);

            //
            // start send thread
            //

            threadName = "TCP-send " + socketAddressString;
            LOG(logLevelDebug, "Starting thread: %s",threadName.c_str());

            _sendThreadId = epicsThreadCreate(threadName.c_str(),
                    epicsThreadPriorityMedium,
                    epicsThreadGetStackSize(epicsThreadStackMedium),
                    BlockingTCPTransport::sendThreadRunner, this);
        }

        void BlockingTCPTransport::clearAndReleaseBuffer() {
            // NOTE: take care that nextMarkerPosition is set right
            // fix position to be correct when buffer is cleared
            // do not include pre-buffered flow control message; not 100% correct, but OK
            _nextMarkerPosition -= _sendBuffer->getPosition() - CA_MESSAGE_HEADER_SIZE;

            _sendQueueMutex.lock();
            _flushRequested = false;
            _sendQueueMutex.unlock();

            _sendBuffer->clear();

            _sendPending = false;

            // prepare ACK marker
            _sendBuffer->putByte(CA_MAGIC);
            _sendBuffer->putByte(CA_VERSION);
            _sendBuffer->putByte(0x01 | _byteOrderFlag); // control data
            _sendBuffer->putByte(1); // marker ACK
            _sendBuffer->putInt(0);
        }

        void BlockingTCPTransport::close(bool force) {
            Lock lock(_mutex);

            // already closed check
            if(_closed.get()) return;
            _closed.set();

            // remove from registry
            Transport::shared_pointer thisSharedPtr = shared_from_this();
            _context->getTransportRegistry()->remove(thisSharedPtr).get();

            // clean resources
            internalClose(force);

            // notify send queue
            _sendQueueEvent.signal();
            
            lock.unlock();
            
            // post close without a lock
            internalPostClose(force);
        }

        void BlockingTCPTransport::internalClose(bool force) {
            // close the socket
            if(_channel!=INVALID_SOCKET) {
                epicsSocketDestroy(_channel);
            }
        }

        void BlockingTCPTransport::internalPostClose(bool force) {
        }

        int BlockingTCPTransport::getSocketReceiveBufferSize() const {
            // Get value of the SO_RCVBUF option for this DatagramSocket,
            // that is the buffer size used by the platform for input on
            // this DatagramSocket.

            int sockBufSize;
            osiSocklen_t intLen = sizeof(int);

            int retval = getsockopt(_channel, SOL_SOCKET, SO_RCVBUF, (char *)&sockBufSize, &intLen);
            if(unlikely(retval<0))
            {
                char errStr[64];
                epicsSocketConvertErrnoToString(errStr, sizeof(errStr));
                LOG(logLevelError,
                    "Socket getsockopt SO_RCVBUF error: %s",
                    errStr);
            }

            return sockBufSize;
        }

        bool BlockingTCPTransport::waitUntilVerified(double timeout) {
            return _verifiedEvent.wait(timeout);
        }

        void BlockingTCPTransport::flush(bool lastMessageCompleted) {

            // automatic end
            endMessage(!lastMessageCompleted);

            bool moreToSend = true;
            // TODO closed check !!!
            while(moreToSend) {
                moreToSend = !flush();

                // all sent, exit
                if(!moreToSend) break;

                // TODO solve this sleep in a better way
                epicsThreadSleep(0.01);
            }

            _lastMessageStartPosition = _sendBuffer->getPosition();
            
            // start with last header
            if (unlikely(!lastMessageCompleted && _lastSegmentedMessageType!=0))
                startMessage(_lastSegmentedMessageCommand, 0);
        }

        void BlockingTCPTransport::startMessage(int8 command, int ensureCapacity) {
            _lastMessageStartPosition = -1;
            ensureBuffer(CA_MESSAGE_HEADER_SIZE+ensureCapacity);
            _lastMessageStartPosition = _sendBuffer->getPosition();
            _sendBuffer->putByte(CA_MAGIC);
            _sendBuffer->putByte(CA_VERSION);
            _sendBuffer->putByte(_lastSegmentedMessageType | _byteOrderFlag); // data + endianess
            _sendBuffer->putByte(command);                   // command
            _sendBuffer->putInt(0);                          // temporary zero payload

        }

        void BlockingTCPTransport::endMessage() {
            endMessage(false);
        }

        void BlockingTCPTransport::ensureBuffer(int size) {
            if(likely((int)(_sendBuffer->getRemaining())>=size)) return;

            // too large for buffer...
            if(unlikely(_maxPayloadSize<size)) {
                ostringstream temp;
                temp<<"requested for buffer size "<<size<<", but only ";
                temp<<_maxPayloadSize<<" available.";
                THROW_BASE_EXCEPTION(temp.str().c_str());
            }

            while(((int)_sendBuffer->getRemaining())<size && !_closed.get())
                flush(false);

            if (unlikely(_closed.get())) THROW_BASE_EXCEPTION("transport closed");
        }
        
        void BlockingTCPTransport::alignBuffer(int alignment) {
            // not space optimal (always requires 7-bytes), but fast
            
            if(unlikely((int)(_sendBuffer->getRemaining())<(alignment-1)))
                ensureBuffer(alignment-1);

            _sendBuffer->align(alignment);
        }
        
        void BlockingTCPTransport::endMessage(bool hasMoreSegments) {
            if(likely(_lastMessageStartPosition>=0)) {
                
    			// align
    //			alignBuffer(CA_ALIGNMENT);
    			
    			// set paylaod size
                _sendBuffer->putInt(_lastMessageStartPosition+sizeof(int16)+2,
                        _sendBuffer->getPosition()-_lastMessageStartPosition
                                -CA_MESSAGE_HEADER_SIZE);
                                
                int flagsPosition = _lastMessageStartPosition+sizeof(int16);
                // set segmented bit
                if(likely(hasMoreSegments)) {
                    // first segment
                    if(unlikely(_lastSegmentedMessageType==0)) {
                        int8 type = _sendBuffer->getByte(flagsPosition);

                        // set first segment bit
                        _sendBuffer->putByte(flagsPosition, (int8)(type|0x10));

                        // first + last segment bit == in-between segment
                        _lastSegmentedMessageType = (int8)(type|0x30);
                        _lastSegmentedMessageCommand = _sendBuffer->getByte(
                                flagsPosition+1);
                    }
                }
                else {
                    // last segment
                    if(unlikely(_lastSegmentedMessageType!=0)) {
                        // set last segment bit (by clearing first segment bit)
                        _sendBuffer->putByte(flagsPosition,
                                (int8)(_lastSegmentedMessageType&0xEF));
                        _lastSegmentedMessageType = 0;
                    }
                }

                // manage markers
                int position = _sendBuffer->getPosition();
                int bytesLeft = _sendBuffer->getRemaining();

                if(unlikely(position>=_nextMarkerPosition && 
                   bytesLeft>=CA_MESSAGE_HEADER_SIZE)) {
                    _sendBuffer->putByte(CA_MAGIC);
                    _sendBuffer->putByte(CA_VERSION);
                    _sendBuffer->putByte(0x01 | _byteOrderFlag); // control data
                    _sendBuffer->putByte(0); // marker
                    _sendBuffer->putInt((int)(_totalBytesSent+position+CA_MESSAGE_HEADER_SIZE));
                    _nextMarkerPosition = position+_markerPeriodBytes;
                }
            }
        }

        void BlockingTCPTransport::ensureData(int size) {
            // enough of data?
            if(likely(((int)_socketBuffer->getRemaining())>=size)) return;

            // too large for buffer...
            if(unlikely(MAX_ENSURE_DATA_BUFFER_SIZE<size)) {
                ostringstream temp;
                temp<<"requested for buffer size "<<size<<", but only ";
                temp<<MAX_ENSURE_DATA_BUFFER_SIZE<<" available.";
                THROW_BASE_EXCEPTION(temp.str().c_str());
            }

            // subtract what was already processed
            _storedPayloadSize -= _socketBuffer->getPosition()-_storedPosition;

            // no more data and we have some payload left => read buffer
            if(likely(_storedPayloadSize>=size)) {
                //LOG(logLevelInfo,
                //        "storedPayloadSize >= size, remaining: %d",
                //        _socketBuffer->getRemaining());

    			// just read up remaining payload, move current (<size) part of the buffer
    			// to the beginning of the buffer
                processReadCached(true, PROCESS_PAYLOAD, size);
                _storedPosition = _socketBuffer->getPosition();
                _storedLimit = _socketBuffer->getLimit();
                _socketBuffer->setLimit(min(_storedPosition+_storedPayloadSize,
                        _storedLimit));
            }
            else {
                // copy remaining bytes, if any
                int remainingBytes = _socketBuffer->getRemaining();
                for(int i = 0; i<remainingBytes; i++)
                    _socketBuffer->putByte(i, _socketBuffer->getByte());

                // read what is left
                _socketBuffer->setLimit(_storedLimit);

                _stage = PROCESS_HEADER;
                processReadCached(true, UNDEFINED_STAGE, size-remainingBytes);

                // copy before position
                for(int i = remainingBytes-1, j = _socketBuffer->getPosition()
                        -1; i>=0; i--, j--)
                    _socketBuffer->putByte(j, _socketBuffer->getByte(i));
                _startPosition = _socketBuffer->getPosition()-remainingBytes;
                _socketBuffer->setPosition(_startPosition);

                _storedPosition = _startPosition; //socketBuffer.position();
                _storedLimit = _socketBuffer->getLimit();
                _socketBuffer->setLimit(min(_storedPosition+_storedPayloadSize,
                        _storedLimit));

                // add if missing...
                if(unlikely(!_closed.get()&&((int)_socketBuffer->getRemaining())<size))
                    ensureData(size);
            }

            if(unlikely(_closed.get())) THROW_BASE_EXCEPTION("transport closed");
        }

        void BlockingTCPTransport::alignData(int alignment) {
            // not space optimal (always requires 7-bytes), but fast
            
            if(unlikely((int)(_socketBuffer->getRemaining())<(alignment-1)))
                ensureData(alignment-1);

            _socketBuffer->align(alignment);
        }

        void BlockingTCPTransport::processReadCached(bool nestedCall,
                ReceiveStage inStage, int requiredBytes) {
            try {
                while(likely(!_closed.get())) {
                    if(_stage==READ_FROM_SOCKET||inStage!=UNDEFINED_STAGE) {

                        // add to bytes read
                        int currentPosition = _socketBuffer->getPosition();
                        _totalBytesReceived += (currentPosition - _startPosition);

    					// preserve alignment
                        int currentStartPosition = _startPosition =
                            MAX_ENSURE_DATA_BUFFER_SIZE; // "TODO uncomment align" + (unsigned int)currentPosition % CA_ALIGNMENT;

                        // copy remaining bytes, if any
                        int remainingBytes = _socketBuffer->getRemaining();
                        int endPosition = currentStartPosition + remainingBytes;
                        // TODO memmove
                        for(int i = MAX_ENSURE_DATA_BUFFER_SIZE; i<endPosition; i++)
                            _socketBuffer->putByte(i, _socketBuffer->getByte());

                        _socketBuffer->setPosition(endPosition);
                        _socketBuffer->setLimit(_socketBuffer->getSize());

                        // read at least requiredBytes bytes

                        uintptr_t requiredPosition = (currentStartPosition+requiredBytes);
                        while(_socketBuffer->getPosition()<requiredPosition) {
                            // read
                            int pos = _socketBuffer->getPosition();
                            ssize_t bytesRead = recv(_channel, (char*)(_socketBuffer->getArray()+pos),
                                                     _socketBuffer->getRemaining(), 0);

                            if(unlikely(bytesRead<=0)) {

                                if (bytesRead<0)
                                {
                                    int socketError = SOCKERRNO;
                                    
                                    // interrupted or timeout
                                    if (socketError == EINTR || 
                                        socketError == EAGAIN ||
                                        socketError == EWOULDBLOCK)
                                        continue;
                                }
                                
                                // error (disconnect, end-of-stream) detected
                                close(true);

                                if(nestedCall)
                                    THROW_BASE_EXCEPTION("bytesRead < 0");

                                return;
                            }
                            
                            _socketBuffer->setPosition(pos+bytesRead);
                        }
                        _socketBuffer->setLimit(_socketBuffer->getPosition());
                        _socketBuffer->setPosition(currentStartPosition);

                        /*
                        hexDump("\n\n\n", "READ",
                                (const int8*)_socketBuffer->getArray(),
                                _socketBuffer->getPosition(), _socketBuffer->getRemaining());
                        */
                        
                        // notify liveness
                        aliveNotification();

                        // exit
                        if(inStage!=UNDEFINED_STAGE) return;

                        _stage = PROCESS_HEADER;
                    }

                    if(likely(_stage==PROCESS_HEADER)) {
                        // ensure CAConstants.CA_MESSAGE_HEADER_SIZE bytes of data
                        if(unlikely(((int)_socketBuffer->getRemaining())<CA_MESSAGE_HEADER_SIZE))
                            processReadCached(true, PROCESS_HEADER, CA_MESSAGE_HEADER_SIZE);

                        // first byte is CA_MAGIC
                        // second byte version - major/minor nibble
                        int8 magic = _socketBuffer->getByte();
                        _version = _socketBuffer->getByte();
                        if(unlikely((magic != CA_MAGIC) || (((uint8_t)_version) >> 4)!=CA_MAJOR_PROTOCOL_REVISION))
                        {
                            // error... disconnect
                            LOG(
                                    logLevelError,
                                    "Invalid header received from client %s, disconnecting...",
                                    inetAddressToString(_socketAddress).c_str());
                            close(true);
                            return;
                        }

                        // data vs. control packet
                        _packetType = _socketBuffer->getByte();

                        // command
                        _command = _socketBuffer->getByte();

                        // read payload size
                        _payloadSize = _socketBuffer->getInt();

                        int8 type = (int8)(_packetType&0x0F);
                        if(likely(type==0))
                        {
                            // data
                            _stage = PROCESS_PAYLOAD;
                        }
                        else if(unlikely(type==1))
                        {
                            // control
                            
                            // marker request sent
                            if (_command == CMD_SET_MARKER) {
                                _flowControlMutex.lock();
                                if(_markerToSend==0)
                                    _markerToSend = _payloadSize;
                                 // TODO send back response
                                _flowControlMutex.unlock();
                            }
                            
                            // marker received back
                            else if (_command == CMD_ACK_MARKER)
                            {
                                _flowControlMutex.lock();
                                int difference = (int)_totalBytesSent-_payloadSize+CA_MESSAGE_HEADER_SIZE;
                                // overrun check
                                if(difference<0) difference += INT_MAX;
                                _remoteBufferFreeSpace
                                        = _remoteTransportReceiveBufferSize
                                                +_remoteTransportSocketReceiveBufferSize
                                                -difference;
                                // TODO if this is calculated wrong, this can be critical !!!
                                _flowControlMutex.unlock();
                            }
    						// set byte order
    						else if (_command == CMD_SET_ENDIANESS)
    						{
    							// check 7-th bit
    							
    							int endianess = (_packetType < 0 ? EPICS_ENDIAN_BIG : EPICS_ENDIAN_LITTLE);
    							_socketBuffer->setEndianess(endianess);
    							
    							// TODO register as TransportSender and add to the queue
    							// current implementation is OK, but not nice
    							_sendQueueMutex.lock();
    							_sendBuffer->setEndianess(endianess);
    							_byteOrderFlag = (endianess == EPICS_ENDIAN_BIG) ? 0x80 : 0x00;
    							_sendQueueMutex.unlock();
    						}                            

                            // no payload
                            //stage = ReceiveStage.PROCESS_HEADER;
                            continue;
                        }
                        else {
                            LOG(
                                    logLevelError,
                                    "Unknown packet type %d, received from client %s, disconnecting...",
                                    type,
                                    inetAddressToString(_socketAddress).c_str());
                            close(true);
                            return;
                        }
                    }

                    if(likely(_stage==PROCESS_PAYLOAD)) {
                        // read header

                        // last segment bit set (means in-between segment or last segment)
                        bool notFirstSegment = (_packetType&0x20)!=0;

                        _storedPayloadSize = _payloadSize;

                        // if segmented, exit reading code
                        if(nestedCall&&notFirstSegment) return;

                        // NOTE: nested data (w/ payload) messages between segmented messages are not supported
                        _storedPosition = _socketBuffer->getPosition();
                        _storedLimit = _socketBuffer->getLimit();
                        _socketBuffer->setLimit(min(_storedPosition+_storedPayloadSize, _storedLimit));
                        try {
                            // handle response
                            Transport::shared_pointer thisPointer = shared_from_this();
                            _responseHandler->handleResponse(&_socketAddress,
                                    thisPointer, _version, _command, _payloadSize,
                                    _socketBuffer);
                        } catch(...) {
                            //noop      // TODO print?
                        }

                        _socketBuffer->setLimit(_storedLimit);
                        int newPosition = _storedPosition+_storedPayloadSize;
                        if(unlikely(newPosition>_storedLimit)) {
                            newPosition -= _storedLimit;
                            _socketBuffer->setPosition(_storedLimit);
                            processReadCached(true, PROCESS_PAYLOAD,newPosition);
                            newPosition += _startPosition;
                        }
                        _socketBuffer->setPosition(newPosition);
                        // TODO discard all possible segments?!!!

                        _stage = PROCESS_HEADER;

                        continue;
                    }

                }
            } catch(...) {
                // close connection
                close(true);

                if(nestedCall) throw;
            }
        }

        bool BlockingTCPTransport::flush() {
            // request issues, has not sent anything yet (per partes)
            if(likely(!_sendPending)) {
                _sendPending = true;

                // start sending from the start
                _sendBufferSentPosition = 0;

                // if not set skip marker otherwise set it
                _flowControlMutex.lock();
                int markerValue = _markerToSend;
                _markerToSend = 0;
                _flowControlMutex.unlock();
                if(markerValue==0)
                    _sendBufferSentPosition = CA_MESSAGE_HEADER_SIZE;
                else
                    _sendBuffer->putInt(4, markerValue);
            }

            bool success = false;
            try {
                // remember current position
                int currentPos = _sendBuffer->getPosition();

                // set to send position
                _sendBuffer->setPosition(_sendBufferSentPosition);
                _sendBuffer->setLimit(currentPos);

                success = send(_sendBuffer);

                // all sent?
                if(likely(success))
                    clearAndReleaseBuffer();
                else {
                    // remember position
                    _sendBufferSentPosition = _sendBuffer->getPosition();

                    // .. reset to previous state
                    _sendBuffer->setPosition(currentPos);
                    _sendBuffer->setLimit(_sendBuffer->getSize());
                }
            //} catch(std::exception& e) {
            //    LOG(logLevelError, "%s", e.what());
            //    // error, release lock
            //    clearAndReleaseBuffer();
            } catch(...) {
                clearAndReleaseBuffer();
            }
            return success;
        }

        bool BlockingTCPTransport::send(ByteBuffer* buffer) {
            try {
                // TODO simply use value from marker???!!!
                // On Windows, limiting the buffer size is important to prevent
                // poor throughput performances when transferring large amount of
                // data. See Microsoft KB article KB823764.
                // We do it also for other systems just to be safe.
                int maxBytesToSend = min(_socketSendBufferSize,
                        _remoteTransportSocketReceiveBufferSize)/2;

                int limit = buffer->getLimit();
                int bytesToSend = limit-buffer->getPosition();

                //LOG(logLevelInfo,"Total bytes to send: %d", bytesToSend);

                // limit sending
                if(bytesToSend>maxBytesToSend) {
                    bytesToSend = maxBytesToSend;
                    buffer->setLimit(buffer->getPosition()+bytesToSend);
                }

                //LOG(logLevelInfo,
                //        "Sending %d of total %d bytes in the packet to %s.",
                //        bytesToSend, limit,
                //        inetAddressToString(_socketAddress).c_str());

                while(buffer->getRemaining()>0) {
                    ssize_t bytesSent = ::send(_channel,
                            &buffer->getArray()[buffer->getPosition()],
                            buffer->getRemaining(), 0);

                    if(unlikely(bytesSent<0)) {
                        
                        int socketError = SOCKERRNO;

                        // spurious EINTR check                     
                        if (socketError==SOCK_EINTR)
                            continue;

                        // TODO check this (copy below)... consolidate!!!
                        if (socketError==SOCK_ENOBUFS) {
                            /* buffers full, reset the limit and indicate that there
                             * is more data to be sent
                             */
                            if(bytesSent==maxBytesToSend) buffer->setLimit(limit);
                            return false;
                        }

                        // connection lost

                        char errStr[64];
                        epicsSocketConvertErrnoToString(errStr, sizeof(errStr));
                        ostringstream temp;
                        temp<<"error in sending TCP data: "<<errStr;
                        //LOG(logLevelError, "%s", temp.str().c_str());
                        THROW_BASE_EXCEPTION(temp.str().c_str());
                    }
                    else if(unlikely(bytesSent==0)) {
                        
                        // TODO WINSOCK indicates disconnect by returning zero here !!!
                        
                        //LOG(logLevelInfo,
                        //        "Buffer full, position %d of total %d bytes.",
                        //        buffer->getPosition(), limit);

                        /* buffers full, reset the limit and indicate that there
                         * is more data to be sent
                         */
                        if(bytesSent==maxBytesToSend) buffer->setLimit(limit);

                        //LOG(logLevelInfo,
                        //        "Send buffer full for %s, waiting...",
                        //        inetAddressToString(_socketAddress));
                        return false;
                    }

                    buffer->setPosition(buffer->getPosition()+bytesSent);

                    _flowControlMutex.lock();
                    _totalBytesSent += bytesSent;
                    _flowControlMutex.unlock();

                    // readjust limit
                    if(bytesToSend==maxBytesToSend) {
                        bytesToSend = limit-buffer->getPosition();
                        if(bytesToSend>maxBytesToSend) bytesToSend
                                = maxBytesToSend;
                        buffer->setLimit(buffer->getPosition()+bytesToSend);
                    }

                    //LOG(logLevelInfo,
                    //        "Sent, position %d of total %d bytes.",
                    //        buffer->getPosition(), limit);
                } // while
            } catch(...) {
                close(true);
                throw;
            }

            // all sent
            return true;
        }

        void BlockingTCPTransport::processSendQueue() {
            while(unlikely(!_closed.get())) {

                _sendQueueMutex.lock();
                // TODO optimize
                TransportSender::shared_pointer sender;
                if (likely(!_sendQueue.empty()))
                {
                    sender = _sendQueue.front();
                    _sendQueue.pop_front();
                }
                _sendQueueMutex.unlock();

                // wait for new message
                while(likely(sender.get()==0&&!_flushRequested&&!_closed.get())) {
                    if(_flushStrategy==DELAYED) {
                        if(_delay>0) epicsThreadSleep(_delay);
                        if(unlikely(_sendQueue.empty())) {
                            // if (hasMonitors || sendBuffer.position() > CAConstants.CA_MESSAGE_HEADER_SIZE)
                            if(((int)_sendBuffer->getPosition())>CA_MESSAGE_HEADER_SIZE)
                                _flushRequested = true;
                            else
                                _sendQueueEvent.wait();
                        }
                    }
                    else
                        _sendQueueEvent.wait();

                    _sendQueueMutex.lock();
                    if (likely(!_sendQueue.empty()))
                    {
                        sender = _sendQueue.front();
                        _sendQueue.pop_front();
                    }
                    else
                        sender.reset();
                    _sendQueueMutex.unlock();
                }

                // always do flush from this thread
                if(unlikely(_flushRequested)) {
                    /*
                     if (hasMonitors)
                     {
                     monitorSender.send(sendBuffer, this);
                     }
                     */

                    flush();
                }

                if(likely(sender.get() != 0)) {
                    sender->lock();
                    try {
                        _lastMessageStartPosition = _sendBuffer->getPosition();
                        sender->send(_sendBuffer, this);

                        if(_flushStrategy==IMMEDIATE)
                            flush(true);
                        else
                            endMessage(false);// automatic end (to set payload)

                    } catch(std::exception &e) {
                        //LOG(logLevelError, "%s", e.what());
                        _sendBuffer->setPosition(_lastMessageStartPosition);
                    } catch(...) {
                        _sendBuffer->setPosition(_lastMessageStartPosition);
                    }
                    sender->unlock();
                } // if(sender!=NULL)
            } // while(!_closed.get())
        }

        void BlockingTCPTransport::freeSendBuffers() {
            // TODO ?
        }

        void BlockingTCPTransport::freeConnectionResorces() {
            freeSendBuffers();

            LOG(logLevelDebug, "Connection to %s closed.",
                    inetAddressToString(_socketAddress).c_str());
/*
            if(_channel!=INVALID_SOCKET) {
                epicsSocketDestroy(_channel);
                _channel = INVALID_SOCKET;
            }
*/
        }

        void BlockingTCPTransport::rcvThreadRunner(void* param) {
            BlockingTCPTransport* obj = (BlockingTCPTransport*)param;
            Transport::shared_pointer ptr = obj->shared_from_this();    // hold reference

try{
            obj->processReadCached(false, UNDEFINED_STAGE, CA_MESSAGE_HEADER_SIZE);
} catch (...) {
printf("rcvThreadRunnner exception\n");
}

            /*
            if(obj->_autoDelete) {
                while(true)
                {
                    bool exited;
                    obj->_mutex.lock();
                    exited = obj->_sendThreadExited;
                    obj->_mutex.unlock();
                    if (exited)
                        break;
                    epicsThreadSleep(0.1);
                }
                delete obj;
            }
             */
        }

        void BlockingTCPTransport::sendThreadRunner(void* param) {
            BlockingTCPTransport* obj = (BlockingTCPTransport*)param;
            Transport::shared_pointer ptr = obj->shared_from_this();    // hold reference
try {
            obj->processSendQueue();
} catch (std::exception& ex) {
    printf("sendThreadRunnner exception %s\n", ex.what()); // TODO
} catch (...) {
printf("sendThreadRunnner exception\n");
}

            obj->freeConnectionResorces();

            // TODO possible crash on unlock
            obj->_mutex.lock();
            obj->_sendThreadExited = true;
            obj->_mutex.unlock();
        }

        void BlockingTCPTransport::enqueueSendRequest(TransportSender::shared_pointer const & sender) {
            Lock lock(_sendQueueMutex);
            if(unlikely(_closed.get())) return;
            _sendQueue.push_back(sender);
            _sendQueueEvent.signal();
        }

        /*
        void BlockingTCPTransport::enqueueMonitorSendRequest(TransportSender::shared_pointer sender) {
            Lock lock(_monitorMutex);
            if(unlikely(_closed.get())) return;
            _monitorSendQueue.insert(sender);
            if(_monitorSendQueue.size()==1) enqueueSendRequest(_monitorSender);
        }
        

        void MonitorSender::send(ByteBuffer* buffer, TransportSendControl* control) {
            control->startMessage(19, 0);

            while(true) {
                TransportSender* sender;
                _monitorMutex->lock();
                if(_monitorSendQueue->size()>0)
                    sender = _monitorSendQueue->extract();
                else
                    sender = NULL;
                _monitorMutex->unlock();

                if(sender==NULL) {
                    control->ensureBuffer(sizeof(int32));
                    buffer->putInt(INVALID_IOID);
                    break;
                }
                sender->send(buffer, control);
                sender->release();
            }
        }
*/
    }
}
