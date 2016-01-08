#include "plugin.h"

#include "_class_BaseInterface.h"
#include "_class_JSONValue.h"

#define JSONVALUE_DEBUG_LOG   1   // if != 0 then log info when reading/writing JSONValue channels.

LxResult JSONValue::val_Copy(ILxUnknownID other)
{
  /*
    Copy another instance of our custom value to this one. We just cast
    the object to our internal structure and then copy the data.
  */

  _JSONValue *otherData = (_JSONValue *)((void *)other);
  if (!otherData) return LXe_FAILED;
  
  m_data.s             = otherData->s;
  m_data.baseInterface = otherData->baseInterface;

  return LXe_OK;
}

LxResult JSONValue::val_GetString(char *buf, unsigned len)
{
  /*
    This function - as the name suggests - is used to get the custom value
    as a string. We just read the string from the custom value object. As
    the caller provides a buffer and length, we should test the length of
    the buffer against our string length, and if it's too short, return
    short buffer. The caller will then provide a bigger buffer for us to
    copy the string into.
  */
  
  if (!buf)       return LXe_FAILED;

  if (m_data.s.size() >= len)
    return LXe_SHORTBUFFER;

  strncpy(buf, m_data.s.c_str(), len);

  return LXe_OK;
}

LxResult JSONValue::val_SetString(const char *val)
{
  /*
    Similar to the get string function, this function sets the string.
  */

  if (val)    m_data.s = val;
  else        m_data.s.clear();
    
  return LXe_OK;
}

void *JSONValue::val_Intrinsic()
{
  /*
    The Intrinsic function is the important one. This returns a pointer
    to the custom value's class, allowing callers to interface with it directly.
  */
  return (void *)&m_data;
}

LxResult JSONValue::io_Write(ILxUnknownID stream)
{
  /*
    The Write function is called whenever the custom value type is being
    written to a stream, for example, writing to a scene file. 

    NOTE: we do not write the string m_data.s, instead we write
          the JSON string BaseInterface::getJSON().

    [FE-4927] unfortunately these types of channels can only store 2^16 bytes,
              so until that limitation is present the JSON string is split
              into chunks of size CHN_FabricJSON_MAX_BYTES and divided over
              all the CHN_NAME_IO_FabricJSON channels.
              Not the prettiest workaround, but it works.
  */
  CLxUser_BlockWrite write(stream);
  char preLog[128];
  sprintf(preLog, "JSONValue::io_Write(m_data.chnIndex = %ld)", m_data.chnIndex);

  if (!write.test())        return LXe_FAILED;
  if (m_data.chnIndex < 0)  return LXe_FAILED;

  // note: we never write nothing, i.e. zero bytes or else
  // the CHN_NAME_IO_FabricJSON channels won't get properly
  // initialized when loading a scene.
  char pseudoNothing[8] = " ";

  // write the JSON string.
  if (!m_data.baseInterface)
  { feLogError(std::string(preLog) + ": pointer at BaseInterface is NULL!");
    return LXe_FAILED;  }
  try
  {
    // get the JSON string and its length.
    std::string json = m_data.baseInterface->getJSON();
    uint32_t len = json.length();

    // trivial case, i.e. nothing to write?
    if ((uint32_t)m_data.chnIndex * CHN_FabricJSON_MAX_BYTES >= len)
      return write.WriteString(pseudoNothing);

    // string too long?
    if (len > (uint32_t)CHN_FabricJSON_NUM * CHN_FabricJSON_MAX_BYTES)
    {
      if (m_data.chnIndex == 0)
      {
        char log[256];
        sprintf(log, " the JSON string is %ld long and exceeds the max size of %ld bytes!", len, (uint32_t)CHN_FabricJSON_NUM * CHN_FabricJSON_MAX_BYTES);
        feLogError(std::string(preLog) + log);
      }
      return LXe_FAILED;
    }

    // extract the part that will be saved for this channel.
    std::string part;
    part = json.substr((uint32_t)m_data.chnIndex * CHN_FabricJSON_MAX_BYTES, CHN_FabricJSON_MAX_BYTES);

    // write.
    if (JSONVALUE_DEBUG_LOG)
    {
      char log[128];
      sprintf(log, " writing %.1f kilobytes (%ld bytes)", (float)part.length() / 1024.0, (long)part.length());
      feLog(std::string(preLog) + log);
    }
    if (part.length() && part.c_str())   return write.WriteString(part.c_str());
    else                                 return write.WriteString(pseudoNothing);
  }
  catch (FabricCore::Exception e)
  {
    std::string err = std::string(preLog) + ": ";
    err += (e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
    feLogError(err);
    return LXe_FAILED;
  }
}

LxResult JSONValue::io_Read(ILxUnknownID stream)
{
  /*
    The Read function is called whenever the custom value type is being
    read from a stream, for example, loading from a scene file. 

    NOTE: the string is read into m_data.s and will then be used in
          the function Instance::pins_AfterLoad() to set the graph
          via BaseInterface::setFromJSON().
  */

  CLxUser_BlockRead read(stream);

  if (read.test() && read.Read(m_data.s))   return LXe_OK;
  else                                      return LXe_FAILED;
}

LXtTagInfoDesc JSONValue::descInfo[] =
{
  { LXsSRV_LOGSUBSYSTEM, LOG_SYSTEM_NAME },
  { 0 }
};
