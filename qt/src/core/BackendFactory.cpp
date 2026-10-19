#include "core/Backend.h"

#include "proto/FtpBackend.h"
#include "proto/LocalBackend.h"
#include "proto/ScpBackend.h"
#include "proto/SftpBackend.h"
#include "proto/WebDavBackend.h"

namespace wsc {

BackendPtr createBackend(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Local:
        return std::make_shared<LocalBackend>();
    case Protocol::Sftp:
        return std::make_shared<SftpBackend>();
    case Protocol::Scp:
        return std::make_shared<ScpBackend>();
    case Protocol::Ftp:
        return std::make_shared<FtpBackend>(Protocol::Ftp);
    case Protocol::Ftps:
        return std::make_shared<FtpBackend>(Protocol::Ftps);
    case Protocol::WebDav:
        return std::make_shared<WebDavBackend>();
    case Protocol::S3:
        return nullptr;
    }
    return nullptr;
}

} // namespace wsc
