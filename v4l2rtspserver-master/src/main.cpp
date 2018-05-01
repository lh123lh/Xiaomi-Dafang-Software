/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** main.cpp
** 
** V4L2 RTSP streamer                                                                 
**                                                                                    
** H264 capture using V4L2                                                            
** RTSP using live555                                                                 
**                                                                                    
** -------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <dirent.h>

#include <sstream>

// libv4l2
#include <linux/videodev2.h>

// live555
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>

// project
#include "logger.h"

#include "V4l2Device.h"
#include "V4l2Capture.h"
#include "V4l2Output.h"

#include "ImpCapture.h"

#include "H264_V4l2DeviceSource.h"
#include "ServerMediaSubsession.h"
#include "UnicastServerMediaSubsession.h"
#include "MulticastServerMediaSubsession.h"
#include "SegmentServerMediaSubsession.h"
#include "HTTPServer.h"

#ifdef HAVE_ALSA
#include "ALSACapture.h"
#endif

// -----------------------------------------
//    signal handler
// -----------------------------------------
char quit = 0;

void sighandler(int n) {
    printf("SIGINT\n");
    quit = 1;
}


// -----------------------------------------
//    create UserAuthenticationDatabase for RTSP server
// -----------------------------------------
UserAuthenticationDatabase *
createUserAuthenticationDatabase(const std::list <std::string> &userPasswordList, const char *realm) {
    UserAuthenticationDatabase *auth = NULL;
    if (userPasswordList.size() > 0) {
        auth = new UserAuthenticationDatabase(realm, (realm != NULL));

        std::list<std::string>::const_iterator it;
        for (it = userPasswordList.begin(); it != userPasswordList.end(); ++it) {
            std::istringstream is(*it);
            std::string user;
            getline(is, user, ':');
            std::string password;
            getline(is, password);
            auth->addUserRecord(user.c_str(), password.c_str());
        }
    }

    return auth;
}

// -----------------------------------------
//    create RTSP server
// -----------------------------------------
RTSPServer *
createRTSPServer(UsageEnvironment &env, unsigned short rtspPort, unsigned short rtspOverHTTPPort, int timeout,
                 unsigned int hlsSegment, const std::list <std::string> &userPasswordList, const char *realm) {
    UserAuthenticationDatabase *auth = createUserAuthenticationDatabase(userPasswordList, realm);
    RTSPServer *rtspServer = HTTPServer::createNew(env, rtspPort, auth, timeout, hlsSegment);
    if (rtspServer != NULL) {
        // set http tunneling
        if (rtspOverHTTPPort) {
            rtspServer->setUpTunnelingOverHTTP(rtspOverHTTPPort);
        }
    }
    return rtspServer;
}


// -----------------------------------------
//    create FramedSource server
// -----------------------------------------
FramedSource *
createFramedSource(UsageEnvironment *env, int format, DeviceInterface *videoCapture, int outfd, int queueSize,
                   bool useThread, bool repeatConfig, MPEG2TransportStreamFromESSource *muxer) {
    bool muxTS = (muxer != NULL);
    FramedSource *source = NULL;
    if (format == V4L2_PIX_FMT_H264) {
        source = H264_V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread, repeatConfig, muxTS);
        if (muxTS) {
            muxer->addNewVideoSource(source, 5);
            source = muxer;
        }
    } else if (format == V4L2_PIX_FMT_HEVC) {
        source = H265_V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread, repeatConfig, muxTS);
        if (muxTS) {
            muxer->addNewVideoSource(source, 6);
            source = muxer;
        }
    } else {
        source = V4L2DeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread);
        //source = ImpJpegDeviceSource::createNew(*env, videoCapture, outfd, queueSize, useThread);
    }
    return source;
}

// -----------------------------------------
//    add an RTSP session
// -----------------------------------------
int addSession(RTSPServer *rtspServer, const std::string &sessionName,
               const std::list<ServerMediaSubsession *> &subSession) {
    int nbSubsession = 0;
    if (subSession.empty() == false) {
        UsageEnvironment &env(rtspServer->envir());
        ServerMediaSession *sms = ServerMediaSession::createNew(env, sessionName.c_str());
        if (sms != NULL) {
            std::list<ServerMediaSubsession *>::const_iterator subIt;
            for (subIt = subSession.begin(); subIt != subSession.end(); ++subIt) {
                sms->addSubsession(*subIt);
                nbSubsession++;
            }

            rtspServer->addServerMediaSession(sms);

            char *url = rtspServer->rtspURL(sms);
            if (url != NULL) {
                LOG(NOTICE) << "Play this stream using the URL \"" << url << "\"" << std::endl;;
                delete[] url;
            }
        }
    }
    return nbSubsession;
}

// -----------------------------------------
//    convert V4L2 pix format to RTP mime
// -----------------------------------------
std::string getRtpFormat(int format, bool muxTS) {
    std::string rtpFormat;
    if (muxTS) {
        rtpFormat = "video/MP2T";
    } else {
        switch (format) {
            case V4L2_PIX_FMT_HEVC :
                rtpFormat = "video/H265";
                break;
            case V4L2_PIX_FMT_H264 :
                rtpFormat = "video/H264";
                break;
            case V4L2_PIX_FMT_MJPEG:
                rtpFormat = "video/JPEG";
                break;
            case V4L2_PIX_FMT_JPEG :
                rtpFormat = "video/JPEG";
                break;
            case V4L2_PIX_FMT_VP8  :
                rtpFormat = "video/VP8";
                break;
            case V4L2_PIX_FMT_VP9  :
                rtpFormat = "video/VP9";
                break;
        }
    }

    return rtpFormat;
}

// -----------------------------------------
//    convert string video format to fourcc 
// -----------------------------------------
int decodeVideoFormat(const char *fmt) {
    char fourcc[4];
    memset(&fourcc, 0, sizeof(fourcc));
    if (fmt != NULL) {
        strncpy(fourcc, fmt, 4);
    }
    return v4l2_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

// -----------------------------------------
//    convert string audio format to pcm
// -----------------------------------------
#ifdef HAVE_ALSA
/*
snd_pcm_format_t decodeAudioFormat(const std::string& fmt)
{
    snd_pcm_format_t audioFmt = SND_PCM_FORMAT_UNKNOWN;
    if (fmt == "S16_BE") {
        audioFmt = SND_PCM_FORMAT_S16_BE;
    } else if (fmt == "S16_LE") {
        audioFmt = SND_PCM_FORMAT_S16_LE;
    } else if (fmt == "S32_BE") {
        audioFmt = SND_PCM_FORMAT_S32_BE;
    } else if (fmt == "S32_LE") {
        audioFmt = SND_PCM_FORMAT_S32_LE;
    } else if (fmt == "U8") {
        audioFmt = SND_PCM_FORMAT_U8;
    } else if (fmt == "S8") {
        audioFmt = SND_PCM_FORMAT_S8;
    }
    return audioFmt;
}
*/
#endif

// -------------------------------------------------------
//    decode multicast url <group>:<rtp_port>:<rtcp_port>
// -------------------------------------------------------
void decodeMulticastUrl(const std::string &maddr, in_addr &destinationAddress, unsigned short &rtpPortNum,
                        unsigned short &rtcpPortNum) {
    std::istringstream is(maddr);
    std::string ip;
    getline(is, ip, ':');
    if (!ip.empty()) {
        destinationAddress.s_addr = inet_addr(ip.c_str());
    }

    std::string port;
    getline(is, port, ':');
    rtpPortNum = 20000;
    if (!port.empty()) {
        rtpPortNum = atoi(port.c_str());
    }
    rtcpPortNum = rtpPortNum + 1;
}

// -------------------------------------------------------
//    split video,audio device
// -------------------------------------------------------
void decodeDevice(const std::string &device, std::string &videoDev, std::string &audioDev) {
    std::istringstream is(device);
    getline(is, videoDev, ',');
    getline(is, audioDev);
}

std::string getDeviceName(const std::string &devicePath) {
    std::string deviceName(devicePath);
    size_t pos = deviceName.find_last_of('/');
    if (pos != std::string::npos) {
        deviceName.erase(0, pos + 1);
    }
    return deviceName;
}


/* ---------------------------------------------------------------------------
**  get a "deviceid" from uevent sys file
** -------------------------------------------------------------------------*/
#ifdef HAVE_ALSA
/*
std::string getDeviceId(const std::string& evt) {
    std::string deviceid;
    std::istringstream f(evt);
    std::string key;
    while (getline(f, key, '=')) {
            std::string value;
        if (getline(f, value)) {
            if ( (key =="PRODUCT") || (key == "PCI_SUBSYS_ID") ) {
                deviceid = value;
                break;
            }
        }
    }
    return deviceid;
}

std::string  getV4l2Alsa(const std::string& v4l2device) {
    std::string audioDevice(v4l2device);

    std::map<std::string,std::string> videodevices;
    std::string video4linuxPath("/sys/class/video4linux");
    DIR *dp = opendir(video4linuxPath.c_str());
    if (dp != NULL) {
        struct dirent *entry = NULL;
        while((entry = readdir(dp))) {
            std::string devicename;
            std::string deviceid;
            if (strstr(entry->d_name,"video") == entry->d_name) {
                std::string ueventPath(video4linuxPath);
                ueventPath.append("/").append(entry->d_name).append("/device/uevent");
                std::ifstream ifsd(ueventPath.c_str());
                deviceid = std::string(std::istreambuf_iterator<char>{ifsd}, {});
                deviceid.erase(deviceid.find_last_not_of("\n")+1);
            }

            if (!deviceid.empty()) {
                videodevices[entry->d_name] = getDeviceId(deviceid);
            }
        }
        closedir(dp);
    }

    std::map<std::string,std::string> audiodevices;
    int rcard = -1;
    while ( (snd_card_next(&rcard) == 0) && (rcard>=0) ) {
        void **hints = NULL;
        if (snd_device_name_hint(rcard, "pcm", &hints) >= 0) {
            void **str = hints;
            while (*str) {
                std::ostringstream os;
                os << "/sys/class/sound/card" << rcard << "/device/uevent";

                std::ifstream ifs(os.str().c_str());
                std::string deviceid = std::string(std::istreambuf_iterator<char>{ifs}, {});
                deviceid.erase(deviceid.find_last_not_of("\n")+1);
                deviceid = getDeviceId(deviceid);

                if (!deviceid.empty()) {
                    if (audiodevices.find(deviceid) == audiodevices.end()) {
                        std::string audioname = snd_device_name_get_hint(*str, "NAME");
                        audiodevices[deviceid] = audioname;
                    }
                }

                str++;
            }

            snd_device_name_free_hint(hints);
        }
    }

    auto deviceId  = videodevices.find(getDeviceName(v4l2device));
    if (deviceId != videodevices.end()) {
        auto audioDeviceIt = audiodevices.find(deviceId->second);

        if (audioDeviceIt != audiodevices.end()) {
            audioDevice = audioDeviceIt->second;
            std::cout <<  v4l2device << "=>" << audioDevice << std::endl;
        }
    }


    return audioDevice;
}
*/
#endif

// -----------------------------------------
//    entry point
// -----------------------------------------
int main(int argc, char **argv) {
    // default parameters
    const char *dev_name = "/dev/video0";
    bool disableAudio = false;
    int format = V4L2_PIX_FMT_H264;
    int width = 1280;
    int height = 720;
    int queueSize = 10;
    int fps = 25;
    unsigned short rtspPort = 8554;
    unsigned short rtspOverHTTPPort = 0;
    bool multicast = false;
    int verbose = 0;
    std::string outputFile;
    std::string url = "unicast";
    std::string murl = "multicast";
    bool useThread = true;
    std::string maddr;
    bool repeatConfig = true;
    int timeout = 65;
    bool muxTS = false;
    int defaultHlsSegment = 5;
    unsigned int hlsSegment = 0;
    const char *realm = NULL;
    std::list <std::string> userPasswordList;
    int inAudioFreq = 48000;
    int outAudioFreq = 48000;
    audioencoding encode = ENCODE_OPUS;

    int audioNbChannels = 1;
#ifdef HAVE_ALSA
    //snd_pcm_format_t audioFmt = SND_PCM_FORMAT_S16_BE;
#endif
    const char *defaultPort = getenv("PORT");
    if (defaultPort != NULL) {
        rtspPort = atoi(defaultPort);
    }

    // decode parameters
    int c = 0;
    while ((c = getopt(argc, argv, "v::Q:O:" "I:P:p:m:u:M:ct:TS::" "R:U:" "nrwsf::F:W:H:" "AC:a:" "Vh")) != -1) {
        switch (c) {
            case 'v':
                verbose = 1;
                if (optarg && *optarg == 'v') verbose++;
                break;
            case 'Q':
                queueSize = atoi(optarg);
                break;
            case 'O':
                outputFile = optarg;
                break;

                // RTSP/RTP
            case 'I':
                ReceivingInterfaceAddr = inet_addr(optarg);
                break;
            case 'P':
                rtspPort = atoi(optarg);
                break;
            case 'p':
                rtspOverHTTPPort = atoi(optarg);
                break;
            case 'u':
                url = optarg;
                break;
            case 'm':
                multicast = true;
                murl = optarg;
                break;
            case 'M':
                multicast = true;
                maddr = optarg;
                break;
            case 'c':
                repeatConfig = false;
                break;
            case 't':
                timeout = atoi(optarg);
                break;
            case 'T':
                muxTS = true;
                break;
            case 'S':
                hlsSegment = optarg ? atoi(optarg) : defaultHlsSegment;
                muxTS = true;
                break;

                // users
            case 'R':
                realm = optarg;
                break;
            case 'U':
                userPasswordList.push_back(optarg);
                break;
            case 's':
                useThread = false;
                break;
            case 'f':
                format = decodeVideoFormat(optarg);
                break;
            case 'F':
                fps = atoi(optarg);
                break;
            case 'W':
                width = atoi(optarg);
                break;
            case 'H':
                height = atoi(optarg);
                break;

                // ALSA
#ifdef HAVE_ALSA
            case 'A':	disableAudio = true; break;
            case 'C':	audioNbChannels = atoi(optarg); break;
            //case 'a':	audioFmt = decodeAudioFormat(optarg); break;
#endif

                // version
            case 'V':
                std::cout << VERSION << std::endl;
                exit(0);
                break;

                // help
            case 'h':
            default: {
                std::cout << argv[0] << " [-v[v]] [-Q queueSize] [-O file]" << std::endl;
                std::cout
                        << "\t          [-I interface] [-P RTSP port] [-p RTSP/HTTP port] [-m multicast url] [-u unicast url] [-M multicast addr] [-c] [-t timeout] [-T] [-S[duration]]"
                        << std::endl;
                std::cout << "\t          [-r] [-w] [-s] [-f[format] [-W width] [-H height] [-F fps] [device] [device]"
                          << std::endl;
                std::cout << "\t -v        : verbose" << std::endl;
                std::cout << "\t -vv       : very verbose" << std::endl;
                std::cout << "\t -Q length : Number of frame queue  (default " << queueSize << ")" << std::endl;
                std::cout << "\t -O output : Copy captured frame to stdout" << std::endl;

                std::cout << "\t RTSP/RTP options :" << std::endl;
                std::cout << "\t -I addr   : RTSP interface (default autodetect)" << std::endl;
                std::cout << "\t -P port   : RTSP port (default " << rtspPort << ")" << std::endl;
                std::cout << "\t -p port   : RTSP over HTTP port (default " << rtspOverHTTPPort << ")" << std::endl;
                std::cout << "\t -U user:password : RTSP user and password" << std::endl;
                std::cout << "\t -R realm  : use md5 password 'md5(<username>:<realm>:<password>')" << std::endl;
                std::cout << "\t -u url    : unicast url (default " << url << ")" << std::endl;
                std::cout << "\t -m url    : multicast url (default " << murl << ")" << std::endl;
                std::cout << "\t -M addr   : multicast group:port (default is random_address:20000)" << std::endl;
                std::cout << "\t -c        : don't repeat config (default repeat config before IDR frame)" << std::endl;
                std::cout << "\t -t timeout: RTCP expiration timeout in seconds (default " << timeout << ")"
                          << std::endl;
                std::cout << "\t -T        : send Transport Stream instead of elementary Stream" << std::endl;
                std::cout << "\t -S[duration]: enable HLS & MPEG-DASH with segment duration  in seconds (default "
                          << defaultHlsSegment << ")" << std::endl;

                std::cout << "\t -fformat  : V4L2 capture using format (-W,-H,-F are used)" << std::endl;
                std::cout << "\t -W width  : V4L2 capture width (default " << width << ")" << std::endl;
                std::cout << "\t -H height : V4L2 capture height (default " << height << ")" << std::endl;
                std::cout << "\t -F fps    : V4L2 capture framerate (default " << fps << ")" << std::endl;

                std::cout << "\t Sound options :" << std::endl;
                std::cout << "\t -A freq    : Disable Sound"
                          << std::endl;
                exit(0);
            }
        }
    }
    std::list <std::string> devList;
    while (optind < argc) {
        devList.push_back(argv[optind]);
        optind++;
    }
    if (devList.empty()) {
        devList.push_back(dev_name);
    }

    // init logger
    initLogger(verbose);

    // create live555 environment
    TaskScheduler *scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);

    // split multicast info
    struct in_addr destinationAddress;
    destinationAddress.s_addr = chooseRandomIPv4SSMAddress(*env);
    unsigned short rtpPortNum = 20000;
    unsigned short rtcpPortNum = rtpPortNum + 1;
    unsigned char ttl = 5;
    decodeMulticastUrl(maddr, destinationAddress, rtpPortNum, rtcpPortNum);

    // create RTSP server
    RTSPServer *rtspServer = createRTSPServer(*env, rtspPort, rtspOverHTTPPort, timeout, hlsSegment, userPasswordList,
                                              realm);
    if (rtspServer == NULL) {
        LOG(ERROR) << "Failed to create RTSP server: " << env->getResultMsg();
    } else {
        V4l2Output *out = NULL;
        int nbSource = 0;


        std::string baseUrl;

        MPEG2TransportStreamFromESSource *muxer = NULL;
        if (muxTS) {
            muxer = MPEG2TransportStreamFromESSource::createNew(*env);
        }
        StreamReplicator *videoReplicator = NULL;
        std::string rtpFormat;


        int outfd = -1;
        //int videoFormat = V4L2_PIX_FMT_MJPEG;
        int videoFormat = format;


        impParams params;
        params.width = width;
        params.height = height;
        if (videoFormat == V4L2_PIX_FMT_MJPEG) {
            params.mode = IMP_MODE_JPEG;
            OutPacketBuffer::maxSize = 250000;


        } else if (videoFormat == V4L2_PIX_FMT_H264) {
            params.mode = IMP_MODE_H264_SNAP;
            MPEG2TransportStreamFromESSource::maxInputESFrameSize += 4820;
            OutPacketBuffer::maxSize = 300000;

        } else {
            LOG(FATAL) << "Unrecognized Format ";
            exit(0);
        }
        if(width == 1920 && height == 1080){
            OutPacketBuffer::maxSize = 500000;
        }


        params.framerate = fps;
        params.bitrate = 5000;


        ImpCapture *impCapture = new ImpCapture(params);

        if (!outputFile.empty()) {
            if(strcmp(outputFile.c_str(),"memory")==0){
                outfd = -2;
            }else{
                outfd = (int)fopen(outputFile.c_str(),"w");
            }
        }




        rtpFormat.assign(getRtpFormat(videoFormat, muxTS));
        FramedSource *videoSource = createFramedSource(env, videoFormat,
                                                       new DeviceCaptureAccess<ImpCapture>(impCapture),
                                                       outfd, queueSize, useThread, repeatConfig, muxer);
        if (videoSource == NULL) {
            LOG(FATAL) << "Unable to create source for device ";
        } else {
            // extend buffer size if needed
            /*
            if (videoCapture->getBufferSize() > OutPacketBuffer::maxSize) {
                OutPacketBuffer::maxSize = videoCapture->getBufferSize();
            }
             */
            videoReplicator = StreamReplicator::createNew(*env, videoSource, false);
        }



        // Init Audio Capture
        StreamReplicator *audioReplicator = NULL;
        std::string rtpAudioFormat;
#ifdef HAVE_ALSA
        std::string audioDev="/dev/dsp";
        //audioDev = "";
        if (disableAudio == false)
        {
            // find the ALSA device associated with the V4L2 device
            //audioDev = "";

            // Init audio capture
            LOG(NOTICE) << "Create ALSA Source..." << audioDev;

            ALSACaptureParameters param(audioDev.c_str(), inAudioFreq, outAudioFreq, verbose, encode);
            ALSACapture* audioCapture = ALSACapture::createNew(param);
            if (audioCapture)
            {
                FramedSource* audioSource = V4L2DeviceSource::createNew(*env, new DeviceCaptureAccess<ALSACapture>(audioCapture), -1, queueSize, useThread);
                if (audioSource == NULL)
                {
                    LOG(FATAL) << "Unable to create source for device " << audioDev;
                    delete audioCapture;
                }
                else
                {
                    std::ostringstream os;
                    switch (encode)
                    {
                        case ENCODE_MP3:
                            os << "audio/MPEG";
                            break;
                        case ENCODE_OPUS:
                            outAudioFreq = 48000;
                            os << "audio/OPUS/" << outAudioFreq << "/1";
                            break;
                        case ENCODE_PCM:
                            outAudioFreq = inAudioFreq;
                            os << "audio/L16/" << outAudioFreq << "/1";
                            break;
                    }
                    //os << "audio/L16/" << audioCapture->getSampleRate() << "/" << audioCapture->getChannels();
                    //os << "audio/L16/8000/1";
                    //os << "audio/MPEG";
                    rtpAudioFormat.assign(os.str());

                    // extend buffer size if needed
                    if (audioCapture->getBufferSize() > OutPacketBuffer::maxSize)
                    {
                        OutPacketBuffer::maxSize = audioCapture->getBufferSize();
                    }
                    audioReplicator = StreamReplicator::createNew(*env, audioSource, false);
                }
            }
        }
#endif


        // Create Multicast Session
        if (multicast) {
            LOG(NOTICE) << "RTP  address " << inet_ntoa(destinationAddress) << ":" << rtpPortNum;
            LOG(NOTICE) << "RTCP address " << inet_ntoa(destinationAddress) << ":" << rtcpPortNum;

            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(
                        MulticastServerMediaSubsession::createNew(*env, destinationAddress, Port(rtpPortNum),
                                                                  Port(rtcpPortNum), ttl, videoReplicator, rtpFormat));
                // increment ports for next sessions
                rtpPortNum += 2;
                rtcpPortNum += 2;
            }

            if (audioReplicator) {
                subSession.push_back(
                        MulticastServerMediaSubsession::createNew(*env, destinationAddress, Port(rtpPortNum),
                                                                  Port(rtcpPortNum), ttl, audioReplicator,
                                                                  rtpAudioFormat));

                // increment ports for next sessions
                rtpPortNum += 2;
                rtcpPortNum += 2;
            }
            nbSource += addSession(rtspServer, baseUrl + murl, subSession);
        }
        // Create Unicast Session
        if (hlsSegment > 0) {
            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(HLSServerMediaSubsession::createNew(*env, videoReplicator, rtpFormat, hlsSegment));
            }
            nbSource += addSession(rtspServer, baseUrl + url, subSession);

            struct in_addr ip;
            ip.s_addr = ourIPAddress(*env);
            LOG(NOTICE) << "HLS       http://" << inet_ntoa(ip) << ":" << rtspPort << "/" << baseUrl + url << ".m3u8";
            LOG(NOTICE) << "MPEG-DASH http://" << inet_ntoa(ip) << ":" << rtspPort << "/" << baseUrl + url << ".mpd";
        } else {
            std::list < ServerMediaSubsession * > subSession;
            if (videoReplicator) {
                subSession.push_back(UnicastServerMediaSubsession::createNew(*env, videoReplicator, rtpFormat));
            }
            if (audioReplicator) {
                subSession.push_back(UnicastServerMediaSubsession::createNew(*env, audioReplicator, rtpAudioFormat));
            }
            nbSource += addSession(rtspServer, baseUrl + url, subSession);
        }


        if (nbSource > 0) {
            // main loop
            signal(SIGINT, sighandler);
            env->taskScheduler().doEventLoop(&quit);
            LOG(NOTICE) << "Exiting....";
        }

        Medium::close(rtspServer);

        if (out) {
            delete out;
        }
    }

    env->reclaim();
    delete scheduler;

    return 0;
}



