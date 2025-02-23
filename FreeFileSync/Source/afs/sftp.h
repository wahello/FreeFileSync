// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef SFTP_H_5392187498172458215426
#define SFTP_H_5392187498172458215426

#include "abstract.h"


namespace fff
{
bool  acceptsItemPathPhraseSftp(const Zstring& itemPathPhrase); //noexcept
AbstractPath createItemPathSftp(const Zstring& itemPathPhrase); //noexcept

void sftpInit();
void sftpTeardown();

//-------------------------------------------------------

enum class SftpAuthType
{
    password,
    keyFile,
    agent,
};

//use all configuration data that *defines* an SSH session as key when buffering sessions! This is what user expects, e.g. when changing settings in SFTP login dialog
struct SshSessionId
{
    Zstring server;
    int port = 0; // > 0 if set
    Zstring username;
    SftpAuthType authType = SftpAuthType::password;
    Zstring password;           //authType == password or keyFile
    Zstring privateKeyFilePath; //authType == keyFile: use PEM-encoded private key (protected by password) for authentication
    bool allowZlib = false;
};
const int DEFAULT_PORT_SFTP = 22;
//SFTP default port: 22, see %WINDIR%\system32\drivers\etc\services
//=> we could use the "ssh" alias, but let's be explicit

struct SftpLogin : SshSessionId
{
    //other settings not specific to SFTP session:
    int timeoutSec = 15;                    //valid range: [1, inf)
    int traverserChannelsPerConnection = 1; //valid range: [1, inf)
};
AfsDevice condenseToSftpDevice(const SftpLogin& login); //noexcept; potentially messy user input
SftpLogin extractSftpLogin(const AfsDevice& afsDevice); //noexcept

int getServerMaxChannelsPerConnection(const SftpLogin& login); //throw FileError

AfsPath getSftpHomePath(const SftpLogin& login); //throw FileError
}

#endif //SFTP_H_5392187498172458215426
