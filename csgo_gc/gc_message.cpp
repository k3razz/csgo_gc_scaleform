#include "stdafx.h"
#include "gc_message.h"

GCMessageRead::GCMessageRead(uint32_t type, const void *data, uint32_t size)
    : m_data{ static_cast<const uint8_t *>(data) }
    , m_size{ size }
{
    m_type = ReadUint32();
    if (!IsValid())
    {
        assert(false);
        return;
    }

    // override the type if provided
    if (type)
    {
        m_type = type;
    }

    if (IsProtobuf())
    {
        // reading a ProtoMsgHeader
        uint32_t headerSize = ReadUint32();
        if (headerSize)
        {
            CMsgProtoBufHeader header;
            const void *headerData = ReadData(headerSize);
            if (!header.ParseFromArray(headerData, headerSize))
            {
                assert(false);
                m_error = true;
                return;
            }

            assert(header.client_steam_id() == 0);
            assert(header.client_session_id() == 0);
            assert(header.source_app_id() == 0);
            assert(header.job_id_source() != JobIdInvalid);
            assert(header.job_id_target() == JobIdInvalid);
            assert(header.target_job_name() == "");
            assert(header.eresult() == 2);
            assert(header.error_message() == "");
            assert(header.ip() == 0);
            assert(header.gc_msg_src() == 0);
            assert(header.gc_dir_index_source() == 0);

            m_jobId = header.job_id_source();
        }
    }
    else
    {
        // reading a GameStructMsgHeader (GCMsgHdrEx_t = 34 bytes total):
        //   uint32  m_eMsg            ← already consumed above (offset 0)
        //   uint32  m_nSrcGCDirIndex  (offset 4)
        //   uint64  m_ulSteamID       (offset 8)
        //   uint16  m_nHdrVersion     (offset 16)
        //   uint64  m_JobIDTarget     (offset 18) ← keyId for UnlockCrate
        //   uint64  m_JobIDSource     (offset 26) ← save as m_jobId for response routing
        
        // Log the raw bytes for debugging
        if (size >= 34)
        {
            Platform::Print("[STRUCT MSG %u] Raw 34 bytes from offset 0: ", m_type);
            for (int i = 0; i < 34; i++)
            {
                Platform::Print("%02X ", m_data[i]);
            }
            Platform::Print("\n");
        }
        
        ReadUint32();                    // m_nSrcGCDirIndex
        ReadUint64();                    // m_ulSteamID
        ReadUint16();                    // m_nHdrVersion
        m_jobIdTarget = ReadUint64();    // m_JobIDTarget → keyId for UnlockCrate
        m_jobId = ReadUint64();          // m_JobIDSource
        
        Platform::Print("[STRUCT MSG %u] Parsed: offset now at %u, jobIdTarget=%llu, jobIdSource=%llu\n", 
                        m_type, m_offset, m_jobIdTarget, m_jobId);
    }

    // caller needs to check for this
    assert(IsValid());
}

const void *GCMessageRead::ReadData(size_t size)
{
    if (m_error)
    {
        // shouldn't get called
        assert(false);
        return nullptr;
    }

    if (m_offset + size > m_size)
    {
        // overflow
        assert(false);
        m_error = true;
        return nullptr;
    }

    const void *result = &m_data[m_offset];
    m_offset += size;
    return result;
}

// mikkotodo fix!!! this function is fucked and broken
std::string_view GCMessageRead::ReadString()
{
    if (m_error)
    {
        // shouldn't get called
        assert(false);
        return {};
    }

    for (uint32_t i = m_offset; i < m_size; i++)
    {
        if (m_data[i] == '\0')
        {
            std::string_view result{ reinterpret_cast<const char *>(&m_data[m_offset]), i - m_offset };
            m_offset += result.size() + 1;
            return result;
        }
    }

    // overflow
    assert(false);
    m_error = true;
    return {};
}

// mikkotodo useful elswhere as well???
static void AppendProtobuf(std::vector<uint8_t> &buffer, const google::protobuf::MessageLite &message)
{
    size_t protobufOffset = buffer.size();
    size_t protobufSize = message.ByteSizeLong();
    buffer.resize(buffer.size() + protobufSize);

    [[maybe_unused]] bool result = message.SerializeToArray(buffer.data() + protobufOffset, protobufSize);
    assert(result);
}

GCMessageWrite::GCMessageWrite(uint32_t type, const google::protobuf::MessageLite &message, uint64_t jobId)
    : m_type{ type | ProtobufMask }
{
    // write the protobuf message hader
    WriteUint32(m_type);

    if (jobId != JobIdInvalid)
    {
        // response to a job
        CMsgProtoBufHeader header;
        header.set_job_id_target(jobId);

        // write the header size and the data
        WriteUint32(header.ByteSizeLong());
        AppendProtobuf(m_buffer, header);
    }
    else
    {
        // no need for a CMsgProtoBufHeader
        WriteUint32(0);
    }

    // append the serialized protobuf message
    AppendProtobuf(m_buffer, message);
}

GCMessageWrite::GCMessageWrite(uint32_t type)
    : m_type{ type }
{
    // GCMsgHdrEx_t layout (packed, 34 bytes total):
    //   uint32  m_eMsg
    //   uint32  m_nSrcGCDirIndex
    //   uint64  m_ulSteamID
    //   uint16  m_nHdrVersion   (must be 1)
    //   uint64  m_JobIDTarget   (JobIdInvalid)
    //   uint64  m_JobIDSource   (JobIdInvalid)
    WriteUint32(m_type);     // m_eMsg
    WriteUint32(0);          // m_nSrcGCDirIndex
    WriteUint64(0);          // m_ulSteamID
    WriteUint16(1);          // m_nHdrVersion = k_nHdrVersion
    WriteUint64(JobIdInvalid); // m_JobIDTarget
    WriteUint64(JobIdInvalid); // m_JobIDSource
}

GCMessageWrite::GCMessageWrite(uint32_t type, uint64_t jobIdSource)
    : m_type{ type }
{
    // GCMsgHdrEx_t layout with source job ID for routing responses back to client:
    //   uint32  m_eMsg
    //   uint32  m_nSrcGCDirIndex
    //   uint64  m_ulSteamID
    //   uint16  m_nHdrVersion   (must be 1)
    //   uint64  m_JobIDTarget   (response targets the requesting job)
    //   uint64  m_JobIDSource   (server's job ID)
    WriteUint32(m_type);            // m_eMsg
    WriteUint32(0);                 // m_nSrcGCDirIndex
    WriteUint64(0);                 // m_ulSteamID
    WriteUint16(1);                 // m_nHdrVersion = k_nHdrVersion
    WriteUint64(jobIdSource);       // m_JobIDTarget = requesting job (for routing)
    WriteUint64(JobIdInvalid);      // m_JobIDSource = no outgoing job
}

GCMessageWrite::GCMessageWrite(const void *data, uint32_t size)
    : m_type{ 0 } // don't know yet
{
    if (size >= sizeof(uint32_t))
    {
        m_type = *reinterpret_cast<const uint32_t *>(data);
    }
    else
    {
        assert(false);
    }

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    m_buffer.assign(bytes, bytes + size);
}

void GCMessageWrite::WriteData(const void *data, uint32_t size)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    m_buffer.insert(m_buffer.end(), bytes, bytes + size);
}
