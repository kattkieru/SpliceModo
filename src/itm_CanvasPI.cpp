#include "plugin.h"

#include "_class_BaseInterface.h"
#include "_class_FabricDFGWidget.h"
#include "_class_JSONValue.h"
#include "_class_ModoTools.h"
#include "itm_CanvasPI.h"
#include "itm_common.h"
#include <Persistence/RTValToJSONEncoder.hpp>

static CLxItemType gItemType_CanvasPI(SERVER_NAME_CanvasPI);

namespace CanvasPI
{
  // user data structure.
  struct piUserData
  {
    BaseInterface                      *baseInterface;      // pointer at BaseInterface.
    _polymesh                           polymesh;           // baked polygon mesh.
    std::vector <ModoTools::UsrChnDef>  usrChan;            // user channels.
    //
    void zero(void)
    {
      polymesh.clear();
      baseInterface = NULL;
      usrChan.clear();
    }
    void clear(void)
    {
      feLog("CanvasPI::piUserData::clear() called");
      if (baseInterface)
      {
        feLog("CanvasPI::piUserData() delete BaseInterface");
        try
        {
          // delete widget and base interface.
          FabricDFGWidget *w = FabricDFGWidget::getWidgetforBaseInterface(baseInterface);
          if (w) delete w;
          delete baseInterface;
          baseInterface = NULL;
        }
        catch (FabricCore::Exception e)
        {
          feLogError(e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
        }
      }
      zero();
    }
  };

/*
 *  We define a class here that defines the channels used to evaluate our surface.
 *  It has some helper functions for reading the channels, but ultimately is just
 *  an object that can be passed around between the modifier and the surface.
 */
 
  class SurfDef
  {
   public:
    SurfDef() { m_userData = NULL; }
    
    LxResult Prepare (CLxUser_Evaluation &eval, ILxUnknownID item_obj, unsigned *evalIndex);
    LxResult Evaluate(CLxUser_Attributes &attr, unsigned evalIndex);
    LxResult Copy    (SurfDef *other);
    int      Compare (SurfDef *other);
    
    piUserData *m_userData;
  };

  LxResult SurfDef::Prepare(CLxUser_Evaluation &eval, ILxUnknownID item_obj, unsigned *evalIndex)
  {
    /*
     *  This function is used to add channels and user channels to a modifier.
     *  The modifier will then call the evaluate function and read the channel
     *  values.
     */

    // init pointer at user data and get the base interface.
    m_userData = NULL;
    BaseInterface *b = GetBaseInterface(item_obj);
    if (!b)
    { feLogError("SurfDef::Prepare(): GetBaseInterface() returned NULL");
      return LXe_INVALIDARG; }

    // check.
    CLxUser_Item item(item_obj);
    if (!eval.test() || !item.test())   return LXe_NOINTERFACE;
    if (!evalIndex)                     return LXe_INVALIDARG;
    
    // set pointer at user data.
    m_userData = GetInstanceUserData(item_obj);
    if (!m_userData)
    { feLogError("SurfDef::Prepare(): GetInstanceUserData(item_obj) returned NULL");
      return LXe_INVALIDARG; }

    // collect all the user channels.
    ModoTools::usrChanCollect(item, m_userData->usrChan);

    // add the fixed input channels to eval.
    *evalIndex = eval.AddChan(item, CHN_NAME_IO_FabricActive, LXfECHAN_READ);
    eval.AddChan(item, CHN_NAME_IO_FabricEval,   LXfECHAN_READ);
    char chnName[128];
    for (int i=0;i<CHN_FabricJSON_NUM;i++)
    {
      sprintf(chnName, "%s%d", CHN_NAME_IO_FabricJSON, i);
      eval.AddChan(item, chnName, LXfECHAN_READ);
    }

    // add the user channels to eval.
    for (unsigned i=0;i<m_userData->usrChan.size();i++)
    {
      ModoTools::UsrChnDef &c = m_userData->usrChan[i];

      unsigned int type;
      if      (b->HasInputPort (c.chan_name.c_str()))     type = LXfECHAN_READ;
      else if (b->HasOutputPort(c.chan_name.c_str()))     type =                 LXfECHAN_WRITE;
      else                                                type = LXfECHAN_READ | LXfECHAN_WRITE;

      c.eval_index = eval.AddChan(item, c.chan_index, type);
    }

    // done.
    return LXe_OK;
  }
  
  LxResult SurfDef::Evaluate(CLxUser_Attributes &attr, unsigned evalIndex)
  {
    // nothing to do?
    if (!attr || !attr.test())
      return LXe_NOINTERFACE;

    //
    if (!m_userData)
    { feLogError("SurfDef::EvaluateMain(): m_userData is NULL");
      return LXe_OK; }
    BaseInterface *b = m_userData->baseInterface;
    if (!b)
    { feLogError("SurfDef::EvaluateMain(): m_userData->baseInterface is NULL");
      return LXe_OK; }

    // [FE-5579]
    // set the base interface's evaluation member so that it doesn't
    // process notifications while the element is being evaluated.
    FTL::AutoSet<bool> isEvaluating( b->m_evaluating, true );

    // refs and pointers.
    FabricCore::Client *client  = b->getClient();
    if (!client)
    { feLogError("SurfDef::EvaluateMain(): getClient() returned NULL");
      return LXe_OK; }
    FabricCore::DFGBinding binding = b->getBinding();
    if (!binding.isValid())
    { feLogError("SurfDef::EvaluateMain(): invalid binding");
      return LXe_OK; }
    FabricCore::DFGExec graph = binding.getExec();
    if (!graph.isValid())
    { feLogError("SurfDef::EvaluateMain(): invalid graph");
      return LXe_OK; }

    // get item.
    CLxUser_Item item((ILxUnknownID)m_userData->baseInterface->m_ILxUnknownID_CanvasPI);
    if (!item.test())
    { feLogError("SurfDef::EvaluateMain(): item.test() failed");
      return LXe_OK; }

    // make ud.polymesh a valid, empty mesh.
    m_userData->polymesh.setEmptyMesh();

    // read the fixed input channels (so that Modo evaluates them)
    // and return early if the FabricActive flag is disabled.
    int FabricActive = attr.Bool(evalIndex++, false);
    int FabricEval   = attr.Int (evalIndex++);
    (void)FabricEval;
    if (!FabricActive)
      return LXe_OK;

    // Fabric Engine (step 1): loop through all the DFG's input ports and set
    //                         their values from the matching Modo user channels.
    {
      try
      {
        for (unsigned int fi=0;fi<graph.getExecPortCount();fi++)
        {
          // if the port has the wrong type then skip it.
          if (graph.getExecPortType(fi) != FabricCore::DFGPortType_In)
            continue;

          // get pointer at matching channel definition.
          const char *portName = graph.getExecPortName(fi);
          bool storable = true;
          ModoTools::UsrChnDef *cd = ModoTools::usrChanGetFromName(portName, m_userData->usrChan);
          if (!cd)
          { std::string err = "(step 1/3) unable to find a user channel that matches the port \"" + std::string(portName) + "\"";
            feLogError(err);
            continue;  }
          if (cd->eval_index < 0)
          { std::string err = "(step 1/3) user channel evaluation index of port \"" + std::string(portName) + "\" is -1";
            feLogError(err);
            continue;  }

          // "DFG port value = item user channel".
          int retGet = 0;
          std::string port__resolvedType = graph.getExecPortResolvedType(fi);
          if      (   port__resolvedType == "Boolean")    {
                                                            bool val = false;
                                                            retGet = ModoTools::GetChannelValueAsBoolean(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgBoolean(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Integer"
                   || port__resolvedType == "SInt8"
                   || port__resolvedType == "SInt16"
                   || port__resolvedType == "SInt32"
                   || port__resolvedType == "SInt64" )    {
                                                            int val = 0;
                                                            retGet = ModoTools::GetChannelValueAsInteger(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgSInt(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Byte"
                   || port__resolvedType == "UInt8"
                   || port__resolvedType == "UInt16"
                   || port__resolvedType == "Count"
                   || port__resolvedType == "Index"
                   || port__resolvedType == "Size"
                   || port__resolvedType == "UInt32"
                   || port__resolvedType == "DataSize"
                   || port__resolvedType == "UInt64" )    {
                                                            int val = 0;
                                                            retGet = ModoTools::GetChannelValueAsInteger(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgUInt(*client, binding, portName, (uint32_t)val);
                                                          }
          else if (   port__resolvedType == "Scalar"
                   || port__resolvedType == "Float32"
                   || port__resolvedType == "Float64" )   {
                                                            double val = 0;
                                                            retGet = ModoTools::GetChannelValueAsFloat(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgFloat(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "String")     {
                                                            std::string val = "";
                                                            retGet = ModoTools::GetChannelValueAsString(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgString(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Quat")       {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsQuaternion(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgQuat(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Vec2")       {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsVector2(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgVec2(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Vec3")       {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsVector3(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgVec3(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Color")      {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsColor(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgColor(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "RGB")        {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsRGB(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgRGB(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "RGBA")       {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsRGBA(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgRGBA(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Mat44")      {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsMatrix44(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgMat44(*client, binding, portName, val);
                                                          }
          else if (   port__resolvedType == "Xfo")        {
                                                            std::vector <double> val;
                                                            retGet = ModoTools::GetChannelValueAsXfo(attr, cd->eval_index, val);
                                                            if (retGet == 0)    BaseInterface::SetValueOfArgXfo(*client, binding, portName, val);
                                                          }
          else
          {
            std::string err = "the port \"" + std::string(portName) + "\" has the unsupported data type \"" + port__resolvedType + "\"";
            feLogError(err);
            continue;
          }

          if( storable ) {
            // Set ports added with a "storable type" as persistable so their values are 
            // exported if saving the graph
            // TODO: handle this in a "clean" way; here we are not in the context of an undo-able command.
            //       We would need that the DFG knows which binding types are "stored" as attributes on the
            //       DCC side and set these as persistable in the source "addPort" command.
            graph.setExecPortMetadata( portName, DFG_METADATA_UIPERSISTVALUE, "true", false /* canUndo */ );
          }

          // error getting value from user channel?
          if (retGet != 0)
          {
            char serr[64];
            snprintf(serr, sizeof(serr), "%d", retGet);
            std::string err = "failed to get value from user channel \"" + std::string(portName) + "\" (returned " + serr + ")";
            continue;
          }
        }
      }
      catch (FabricCore::Exception e)
      {
        std::string s = std::string("SurfDef::EvaluateMain()(step 1): ") + (e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
        feLogError(s);
      }
    }

    // Fabric Engine (step 2): execute the DFG.
    {
      try
      {
        binding.execute();
      }
      catch (FabricCore::Exception e)
      {
        std::string s = std::string("SurfDef::EvaluateMain()(step 2): ") + (e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
        feLogError(s);
      }
    }

    // Fabric Engine (step 3): loop through all the DFG's output ports and set
    //                         the values of the matching Modo user channels.
    {
      try
      {
        for (unsigned int fi=0;fi<graph.getExecPortCount();fi++)
        {
          // if the port has the wrong type then skip it.
          std::string resolvedType = graph.getExecPortResolvedType(fi);
          if (   graph.getExecPortType(fi) != FabricCore::DFGPortType_Out
              || resolvedType              == "PolygonMesh"  )
            continue;

          // get pointer at matching channel definition.
          const char *portName = graph.getExecPortName(fi);
          ModoTools::UsrChnDef *cd = ModoTools::usrChanGetFromName(portName, m_userData->usrChan);
          if (!cd)
          { std::string err = "(step 3/3) unable to find a user channel that matches the port \"" + std::string(portName) + "\"";
            feLogError(err);
            continue;  }
          if (cd->eval_index < 0)
          { std::string err = "(step 3/3) user channel evaluation index of port \"" + std::string(portName) + "\" is -1";
            feLogError(err);
            continue;  }

          // "item user channel = DFG port value".
          int dataType = attr.Type(cd->eval_index);
          const char *typeName = NULL;
          if (!LXx_OK(attr.TypeName(cd->eval_index, &typeName)))
            typeName = NULL;
          FabricCore::RTVal rtval;
          int retGet = 0;
          int retSet = LXe_OK;
          if (cd->isSingleton)
          {
            if      (dataType == LXi_TYPE_INTEGER)
            {
              int val;
              retGet = BaseInterface::GetArgValueInteger(binding, portName, val);
              if (retGet == 0)
                retSet = attr.SetInt(cd->eval_index, val);
            }
            else if (dataType == LXi_TYPE_FLOAT)
            {
              double val;
              retGet = BaseInterface::GetArgValueFloat(binding, portName, val);
              if (retGet == 0)
                retSet = attr.SetFlt(cd->eval_index, val);
            }
            else if (dataType == LXi_TYPE_STRING)
            {
              std::string val;
              retGet = BaseInterface::GetArgValueString(binding, portName, val);
              if (retGet == 0)
                retSet = attr.SetString(cd->eval_index, val.c_str());
            }
            else if (dataType == LXi_TYPE_OBJECT && typeName && !strcmp (typeName, LXsTYPE_QUATERNION))
            {
              std::vector <double> val;
              retGet = BaseInterface::GetArgValueQuat(binding, portName, val);
              if (retGet == 0 && val.size() == 4)
              {
                CLxUser_Quaternion usrQuaternion;
                LXtQuaternion      q;
                if (!attr.ObjectRW(cd->eval_index, usrQuaternion) || !usrQuaternion.test())
                { std::string err = "the function ObjectRW() failed for the user channel  \"" + std::string(portName) + "\"";
                  feLogError(err);
                  continue;  }
                for (int i = 0; i < 4; i++)   q[i] = val[i];
                usrQuaternion.SetQuaternion(q);
              }
            }
            else if (dataType == LXi_TYPE_OBJECT && typeName && !strcmp (typeName, LXsTYPE_MATRIX4))
            {
              std::vector <double> val;
              retGet = BaseInterface::GetArgValueMat44(binding, portName, val);
              if (retGet == 0 && val.size() == 16)
              {
                CLxUser_Matrix usrMatrix;
                LXtMatrix4     m44;

                if (!attr.ObjectRW(cd->eval_index, usrMatrix) || !usrMatrix.test())
                { std::string err = "the function ObjectRW() failed for the user channel  \"" + std::string(portName) + "\"";
                  feLogError(err);
                  continue;  }

                for (int j = 0; j < 4; j++)
                  for (int i = 0; i < 4; i++)
                    m44[i][j] = val[j * 4 + i];

                usrMatrix.Set4(m44);
              }
            }
            else
            {
              std::string err;
              const char *typeName = NULL;
              attr.TypeName(cd->eval_index, &typeName);
              if (typeName)   err = "the user channel  \"" + std::string(portName) + "\" has the unsupported data type \"" + typeName + "\"";
              else            err = "the user channel  \"" + std::string(portName) + "\" has the unsupported data type \"NULL\"";
              feLogError(err);
              continue;
            }
          }
          else
          {
            std::vector <double> val;
            size_t N = 0;
            if (dataType == LXi_TYPE_FLOAT)
            {
              if      (cd->isVec2x)     {   N = 2;  retGet = BaseInterface::GetArgValueVec2(binding, portName, val);   }
              else if (cd->isVec3x)     {   N = 3;  retGet = BaseInterface::GetArgValueVec3(binding, portName, val);   }
              else if (cd->isRGBr)      {   N = 3;  retGet = BaseInterface::GetArgValueRGB (binding, portName, val);   }
              else if (cd->isRGBAr)     {   N = 4;  retGet = BaseInterface::GetArgValueRGBA(binding, portName, val);   }
              else
              {
                std::string err = "something is wrong with the flags in ModoTools::UsrChnDef";
                feLogError(err);
                continue;
              }

              if (retGet == 0 && val.size() == N)
                for (size_t i = 0; i < N; i++)
                  if (retSet)     break;
                  else            retSet = attr.SetFlt(cd->eval_index + i, val[i]);
            }
          }

          // error getting value from DFG port?
          if (retGet != 0)
          {
            char serr[64];
            snprintf(serr, sizeof(serr), "%d", retGet);
            std::string err = "failed to get value from DFG port \"" + std::string(portName) + "\" (returned " + serr + ")";
            continue;
          }

          // error setting value of user channel?
          if (retSet != 0)
          {
            char serr[64];
            snprintf(serr, sizeof(serr), "%d", retGet);
            std::string err = "failed to set value of user channel \"" + std::string(portName) + "\" (returned " + serr + ")";
            continue;
          }
        }
      }
      catch (FabricCore::Exception e)
      {
        std::string s = std::string("SurfDef::EvaluateMain()(step 3): ") + (e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
        feLogError(s);
      }
    }

    // Fabric Engine (step 4): find all the PolygonMesh output ports and merge
    //                         them into m_userData->polymesh.
    {
      try
      {
        char        serr[256];
        std::string err = "";

        for (unsigned int fi=0;fi<graph.getExecPortCount();fi++)
        {
          // if the port has the wrong type then skip it.
          std::string resolvedType = graph.getExecPortResolvedType(fi);
          if (   graph.getExecPortType(fi) != FabricCore::DFGPortType_Out
              || resolvedType              != "PolygonMesh"  )
            continue;

          // put the port's polygon mesh in tmpMesh.
          const char *portName = graph.getExecPortName(fi);
          _polymesh tmpMesh;
          int retGet = tmpMesh.setFromDFGArg(binding, portName);
          if (retGet)
          {
            sprintf(serr, "%d", retGet);
            err = "failed to get mesh from DFG port \"" + std::string(portName) + "\" (returned " + serr + ")";
            break;
          }

          // merge tmpMesh into m_userData->polymesh.
          if (!m_userData->polymesh.merge(tmpMesh))
          {
            sprintf(serr, "%d", retGet);
            err = "failed to merge current mesh with mesh from DFG port \"" + std::string(portName) + "\"";
            break;
          }
        }

        // error?
        if (err != "")
        {
          m_userData->polymesh.clear();
          feLogError(err);
          return LXe_OK;
        }
      }
      catch (FabricCore::Exception e)
      {
        m_userData->polymesh.clear();
        std::string s = std::string("SurfDef::EvaluateMain()(step 4): ") + (e.getDesc_cstr() ? e.getDesc_cstr() : "\"\"");
        feLogError(s);
      }
    }

    // done.
    return LXe_OK;
  }

  LxResult SurfDef::Copy(SurfDef *other)
  {
    // This function is used to copy the cached channel values from one
    // surface definition to another. We also copy the cached user channels.
    if (other)
    {
      m_userData = other->m_userData;
      return LXe_OK;
    }
    return LXe_INVALIDARG;
  }

  int SurfDef::Compare(SurfDef *other)
  {
    // This function does a comparison of another SurfDef with this one. It
    // should work like strcmp and return 0 for identical, or -1/1 to imply
    // relative positioning.
    if (other && m_userData != other->m_userData)
        return  1;

    return 0;
  }

  /*
    The SurfElement represents a binned surface. A binned surface is essentially a
    collection of triangles, all sharing the same material tag. The procedural
    surface could be constructed from multiple surface elements or in the simplest
    of cases, a single element. A StringTag interface allows the material tag to
    easily be queried.
  */

  class SurfElement : public CLxImpl_TableauSurface,
                      public CLxImpl_SurfaceBin,
                      public CLxImpl_StringTag
  {
   public:
    static void initialize ()
    {
      CLxGenericPolymorph *srv = NULL;
      srv = new CLxPolymorph                            <SurfElement>;
      srv->AddInterface       (new CLxIfc_TableauSurface<SurfElement>);
      srv->AddInterface       (new CLxIfc_SurfaceBin    <SurfElement>);
      srv->AddInterface       (new CLxIfc_StringTag     <SurfElement>);
      lx::AddSpawner          (SERVER_NAME_CanvasPI ".elmt", srv);
    }
    
    SurfElement()   { m_numOffsets = 0; };
    ~SurfElement()  {};

    LxResult	    surfbin_GetBBox     (LXtBBox *bbox)                                                   LXx_OVERRIDE;

    unsigned int  tsrf_FeatureCount   (LXtID4 type)                                                     LXx_OVERRIDE;
    LxResult      tsrf_FeatureByIndex (LXtID4 type, unsigned int index, const char **name)              LXx_OVERRIDE;
    LxResult      tsrf_Bound          (LXtTableauBox bbox)                                              LXx_OVERRIDE;
    LxResult      tsrf_SetVertex      (ILxUnknownID vdesc_obj)                                          LXx_OVERRIDE;
    LxResult      tsrf_Sample         (const LXtTableauBox bbox, float scale, ILxUnknownID trisoup_obj) LXx_OVERRIDE;
    
    LxResult      stag_Get            (LXtID4 type, const char **tag)                                   LXx_OVERRIDE;

    SurfDef       m_surf_def;

   private:
    int           m_offsets[MAX_NUM_VERTEX_FEATURE_OFFSETS];
    int           m_numOffsets;
  };

  LxResult SurfElement::surfbin_GetBBox(LXtBBox *bbox)
  {
    LXtTableauBox	tBox;
    LxResult result = tsrf_Bound (tBox);

    bbox->min[0] = tBox[0];
    bbox->min[1] = tBox[1];
    bbox->min[2] = tBox[2];

    bbox->max[0] = tBox[3];
    bbox->max[1] = tBox[4];
    bbox->max[2] = tBox[5];

    bbox->extent[0] = bbox->max[0] - bbox->min[0];
    bbox->extent[1] = bbox->max[1] - bbox->min[1];
    bbox->extent[2] = bbox->max[2] - bbox->min[2];

    bbox->center[0] = 0.5f * (bbox->min[0] + bbox->max[0]);
    bbox->center[1] = 0.5f * (bbox->min[1] + bbox->max[1]);
    bbox->center[2] = 0.5f * (bbox->min[2] + bbox->max[2]);

    return result;
  }

  unsigned int SurfElement::tsrf_FeatureCount(LXtID4 type)
  {
    /*
      We only define the required features on our surface. We could return
      things like UVs or weight maps if we have them, but we'll just assume
      the standard set of 4 required features.
    */

    unsigned int count = 0;

    if      (type == LXiTBLX_BASEFEATURE)   count = 4;
    else if (type == LXi_VMAP_TEXTUREUV)    count = 1;
    else if (type == LXiTBLX_DPDU)          count = 1;

    return count;
  }

  LxResult SurfElement::tsrf_FeatureByIndex(LXtID4 type, unsigned int index, const char **name)
  {
    /*
      There are four features that are required; position, object position,
      normal and velocity. We could also return any extras if we wanted,
      but we must provide these.
    */

    if (type == LXiTBLX_BASEFEATURE)
    {
      switch (index)
      {
        case 0:   name[0] = LXsTBLX_FEATURE_POS;      return LXe_OK;
        case 1:   name[0] = LXsTBLX_FEATURE_OBJPOS;   return LXe_OK;
        case 2:   name[0] = LXsTBLX_FEATURE_NORMAL;   return LXe_OK;
        case 3:   name[0] = LXsTBLX_FEATURE_VEL;      return LXe_OK;
        default:                                      return LXe_OUTOFBOUNDS;
      }
    }

    if (type == LXi_VMAP_TEXTUREUV)
    {
      switch (index)
      {
        case 0:   name[0] = VMAPNAME_UV;              return LXe_OK;
        default:                                      return LXe_OUTOFBOUNDS;
      }
    }

    if (type == LXiTBLX_DPDU)
    {
      switch (index)
      {
        case 0:   name[0] = VMAPNAME_UV;              return LXe_OK;
        default:                                      return LXe_OUTOFBOUNDS;
      }
    }

    return LXe_NOTFOUND;
  }

  LxResult SurfElement::tsrf_Bound(LXtTableauBox bbox)
  {
    /*
      This is expected to return a bounding box for the current binned
      element.
    */
    
    if (m_surf_def.m_userData && m_surf_def.m_userData->polymesh.isValid())
    {
      bbox[0] = m_surf_def.m_userData->polymesh.bbox[0];
      bbox[1] = m_surf_def.m_userData->polymesh.bbox[1];
      bbox[2] = m_surf_def.m_userData->polymesh.bbox[2];
      bbox[3] = m_surf_def.m_userData->polymesh.bbox[3];
      bbox[4] = m_surf_def.m_userData->polymesh.bbox[4];
      bbox[5] = m_surf_def.m_userData->polymesh.bbox[5];
    }
    
    return LXe_OK;
  }

  LxResult SurfElement::tsrf_SetVertex(ILxUnknownID vdesc_obj)
  {
    /*
      When we write points into the triangle soup, we write arbitrary
      features into an array. The offset for each feature in the array can
      be queried at this point and cached for use in our Sample function.
     
      FETODO: If you add any more vertex features, such as UVs, this
      function will need modifying.
    */

    CLxUser_TableauVertex vertex;
    const char           *name   = NULL;
    unsigned              offset = 0;

    // init offsets.
    m_numOffsets = 0;
    for (int i=0;i<MAX_NUM_VERTEX_FEATURE_OFFSETS;i++)
      m_offsets[i] = -1;

    // set flags.
    bool hasUVs = (m_surf_def.m_userData && m_surf_def.m_userData->polymesh.hasUVWs());

    // check.
    if (!vertex.set(vdesc_obj))
      return LXe_NOINTERFACE;

    // base features.
    for (int i=0;i<4;i++)
    {
      tsrf_FeatureByIndex(LXiTBLX_BASEFEATURE, i, &name);
      if (LXx_OK(vertex.Lookup(LXiTBLX_BASEFEATURE, name, &offset)))
        m_offsets[m_numOffsets++] = offset;
      else
        m_offsets[m_numOffsets++] = -1;
    }

    // texture coordinates.
    if (hasUVs)
    {
      tsrf_FeatureByIndex(LXi_VMAP_TEXTUREUV, 0, &name);
      if (LXx_OK(vertex.Lookup(LXi_VMAP_TEXTUREUV, name, &offset)))
      {
        m_offsets[m_numOffsets++] = offset;
        tsrf_FeatureByIndex(LXiTBLX_DPDU, 0, &name);
        if (LXx_OK(vertex.Lookup(LXiTBLX_DPDU, name, &offset)))
          m_offsets[m_numOffsets++] = offset;
        else
          m_offsets[m_numOffsets++] = -1;
      }
      else
        m_offsets[m_numOffsets++] = -1;
    }

    // done.
    return LXe_OK;
  }

  LxResult SurfElement::tsrf_Sample(const LXtTableauBox bbox, float scale, ILxUnknownID trisoup_obj)
  {
    /*
     *  The Sample function is used to generate the geometry for this Surface
     *  Element. We basically just insert points directly into the triangle
     *  soup feature array and then build polygons/triangles from points at
     *  specific positions in the array.
     */

    // init ref at user data.
    if (!m_surf_def.m_userData)
      return LXe_FAILED;
    piUserData &ud = *m_surf_def.m_userData;

    // nothing to do?
    if (!ud.polymesh.isValid() || ud.polymesh.isEmpty())
        return LXe_OK;

    // init triangle soup.
    CLxUser_TriangleSoup soup(trisoup_obj);
    if (!soup.test())
      return LXe_NOINTERFACE;

    // return early if the bounding box is not visible.
    if (!soup.TestBox(ud.polymesh.bbox))
        return LXe_OK;

    /*
      We're only generating triangles/polygons in a single segment. If
      something else is being requested, we'll early out.
    */

    if (LXx_FAIL(soup.Segment(1, LXiTBLX_SEG_TRIANGLE)))
      return LXe_OK;

    /*
      Build the geometry.
    */

    // set the Modo geometry from ud.pmesh.
    {
      // init.
      LxResult rc = soup.Segment (1, LXiTBLX_SEG_TRIANGLE);
      if (rc == LXe_FALSE)    return LXe_OK;
      else if (LXx_FAIL (rc)) return rc;

      // build the vertex list.
      {
          const int numVec = 3 * MAX_NUM_VERTEX_FEATURE_OFFSETS;
          float vec[numVec];
          for (int i=0;i<numVec;i++)
            vec[i] = 0;

          bool hasUVs = ud.polymesh.hasUVWs();
          float *vp = ud.polymesh.vertPositions.data();
          float *vn = ud.polymesh.vertNormals  .data();
          float *vu = ud.polymesh.vertUVWs     .data();
          for (int i=0;i<ud.polymesh.numVertices;i++,vp+=3,vn+=3,vu+=3)
          {
            // position.
            if (m_numOffsets > 0)
            {
              vec[m_offsets[0] + 0] = vp[0];
              vec[m_offsets[0] + 1] = vp[1];
              vec[m_offsets[0] + 2] = vp[2];
            }
            // object position.
            if (m_numOffsets > 1)
            {
              // [FE-7139]
              vec[m_offsets[1] + 0] = vp[0];
              vec[m_offsets[1] + 1] = vp[1];
              vec[m_offsets[1] + 2] = vp[2];
            }

            // normal.
            if (m_numOffsets > 2)
            {
              vec[m_offsets[2] + 0] = vn[0];
              vec[m_offsets[2] + 1] = vn[1];
              vec[m_offsets[2] + 2] = vn[2];
            }

            // velocity.
            if (m_numOffsets > 3)
            {
              vec[m_offsets[3] + 0] = 0;
              vec[m_offsets[3] + 1] = 0;
              vec[m_offsets[3] + 2] = 0;
            }

            // texture coordinates.
            if (m_numOffsets > 4)
            {
              if (hasUVs && m_offsets[4] != -1)
              {
                vec[m_offsets[4] + 0] = vu[0];
                vec[m_offsets[4] + 1] = vu[1];
              }
            }

            // add vertex.
            unsigned index;
            soup.Vertex(vec, &index);
          }
      }

      // build triangle list.
      {
          // init pointers at polygon data.
          uint32_t *pn = ud.polymesh.polyNumVertices.data();
          uint32_t *pi = ud.polymesh.polyVertices.data();

          // go.
          for (int i=0;i<ud.polymesh.numPolygons;i++)
          {
              if      (*pn == 3)    soup.Polygon((unsigned int)pi[0], (unsigned int)pi[1], (unsigned int)pi[2]);
              else if (*pn == 4)    soup.Quad   ((unsigned int)pi[0], (unsigned int)pi[1], (unsigned int)pi[2], (unsigned int)pi[3]);
              else if (*pn >= 5)
              {
                // [FABMODO-23] triangulate polygons with five or more vertices.
                for (uint32_t j=2;j<*pn;j++)
                  soup.Polygon((unsigned int)pi[0], (unsigned int)pi[j - 1], (unsigned int)pi[j]);
              }

              // next.
              pi += *pn;
              pn++;
          }
      }
    }

    // done.
    return LXe_OK;
  }
    
  LxResult SurfElement::stag_Get(LXtID4 type, const char **tag)
  {
    /*
      This function is called to get the polygon tag for all polygons inside
      of the bin. We only care about setting the material tag and part tag,
      and we'll set them both to default.
         
      FETODO: If you have some way of defining material tagging through
      fabric engine. You'll want to set the tag here so it can be used for
      texturing.
    */
    
    if (type == LXi_PTAG_MATR || type == LXi_PTAG_PART)
    {
      tag[0] = "Default";
      return LXe_OK;
    }
    
    return LXe_NOTFOUND;
  }

  /*
    The surface itself represents the entire 3D surface. It is composed of
    surface elements, divided up in to bins, based on their material tagging.
    It also has a couple of functions for getting things like the bounding box
    and GL triangle count.
  */

  class Surface : public CLxImpl_Surface
  {
   public:
    static void initialize ()
    {
      CLxGenericPolymorph *srv = NULL;
      srv = new CLxPolymorph                        <Surface>;
      srv->AddInterface       (new CLxIfc_Surface   <Surface>);
      lx::AddSpawner          (SERVER_NAME_CanvasPI ".surf", srv);
    }
    
    LxResult  surf_GetBBox    (LXtBBox *bbox)                                           LXx_OVERRIDE;
    LxResult  surf_BinCount   (unsigned int *count)                                     LXx_OVERRIDE;
    LxResult  surf_BinByIndex (unsigned int index, void **ppvObj)                       LXx_OVERRIDE;
    LxResult  surf_TagCount   (LXtID4 type, unsigned int *count)                        LXx_OVERRIDE;
    LxResult  surf_TagByIndex (LXtID4 type, unsigned int index, const char **stag)      LXx_OVERRIDE;
    LxResult  surf_GLCount    (unsigned int *count)                                     LXx_OVERRIDE;
    
    SurfDef   m_surf_def;
  };

  LxResult Surface::surf_GetBBox(LXtBBox *bbox)
  {
    /*
      This is expected to return a bounding box for the entire surface.
    */
    
    if (bbox && m_surf_def.m_userData && m_surf_def.m_userData->polymesh.isValid())
    {
      float *tBox = m_surf_def.m_userData->polymesh.bbox;

      bbox->min[0] = tBox[0];
      bbox->min[1] = tBox[1];
      bbox->min[2] = tBox[2];

      bbox->max[0] = tBox[3];
      bbox->max[1] = tBox[4];
      bbox->max[2] = tBox[5];

      bbox->extent[0] = bbox->max[0] - bbox->min[0];
      bbox->extent[1] = bbox->max[1] - bbox->min[1];
      bbox->extent[2] = bbox->max[2] - bbox->min[2];

      bbox->center[0] = 0.5f * (bbox->min[0] + bbox->max[0]);
      bbox->center[1] = 0.5f * (bbox->min[1] + bbox->max[1]);
      bbox->center[2] = 0.5f * (bbox->min[2] + bbox->max[2]);
    }
    
    return LXe_OK;
  }

  LxResult Surface::surf_BinCount(unsigned int *count)
  {
    /*
      Surface elements are divided into bins, where each bin is a collection
      of triangles with the same polygon tags. This function returns the
      number of bins our surface is divided into. We only have one bin for
      now, but we could potentially have many, each which different shader
      tree masking.
     
      FETODO: Add code here that returns the correct number of bins. If you
      only want one material tag for texturing, you can just return 1.
    */
    
    count[0] = 1;
    
    return LXe_OK;
  }

  LxResult Surface::surf_BinByIndex(unsigned int index, void **ppvObj)
  {
    /*
      This function is called to get a particular surface bin by index. As
      we only have one bin, we always allocate the same object.
     
      FETODO: If you have more than one bin, you'll need to add the correct
      code for allocating different bins.
    */

    if (index == 0)
    {
      CLxSpawner<SurfElement> spawner(SERVER_NAME_CanvasPI ".elmt");
      SurfElement *element = spawner.Alloc(ppvObj);
      if (element)
      {
        element->m_surf_def.Copy(&m_surf_def);
        return LXe_OK;
      }
    }

    return LXe_FAILED;
  }

  LxResult Surface::surf_TagCount(LXtID4 type, unsigned int *count)
  {
    /*
      This function is called to get the list of polygon tags for all
      polygons on the surface. As we only have one bin and one material
      tag, we return 1 for material and part, and 0 for everything else.
         
      FETODO: If you add more than one bin, this function will potentially
      need changing.
    */
    
    if (type == LXi_PTAG_MATR || type == LXi_PTAG_PART)
      count[0] = 1;
    else
      count[0] = 0;
    
    return LXe_OK;
  }

  LxResult Surface::surf_TagByIndex(LXtID4 type, unsigned int index, const char **stag)
  {
    /*
      This function is called to get the list of polygon tags for all
      polygons on the surface. As we only have one bin with one material
      tag and one part tag, we return the default tag.
         
      FETODO: If you add more than one bin or more than one material tag,
      this function will potentially need changing.
    */
    
    if ((type == LXi_PTAG_MATR || type == LXi_PTAG_PART) && index == 0)
    {
      stag[0] = "Default";

      return LXe_OK;
    }
    
    return LXe_OUTOFBOUNDS;
  }

  LxResult Surface::surf_GLCount(unsigned int *count)
  {
    /*
      This function is called to return the GL count for the surface we're
      generating. The GL count should be the number of triangles generated
      by our surface.
    */
    *count = 0;
    if (m_surf_def.m_userData && m_surf_def.m_userData->polymesh.isValid())
    {
      _polymesh &mesh = m_surf_def.m_userData->polymesh;
      for (int i=0;i<mesh.numPolygons;i++)
      {
        unsigned int num = mesh.polyNumVertices[i];
        if (num >= 3)
          *count += num -2;
      }
    }
    return LXe_OK;
  }
    
  /*
    The instanceable object is spawned by our modifier. It has one task, which is
    to return a surface that matches the current state of the input channels.
  */

  class SurfInst : public CLxImpl_Instanceable
  {
   public:
    static void initialize()
    {
      CLxGenericPolymorph *srv = NULL;
      srv = new CLxPolymorph                            <SurfInst>;
      srv->AddInterface       (new CLxIfc_Instanceable  <SurfInst>);
      lx::AddSpawner          (SERVER_NAME_CanvasPI ".instObj", srv);
    }
    
    LxResult  instable_GetSurface (void **ppvObj)         LXx_OVERRIDE;
    int       instable_Compare    (ILxUnknownID other)    LXx_OVERRIDE;
    
    SurfDef     m_surf_def;
  };

  LxResult SurfInst::instable_GetSurface(void **ppvObj)
  {
    /*
      This function is used to allocate the surface. We also copy the cached
      channels from the surface definition to the Surface.
    */

    CLxSpawner<Surface> spawner(SERVER_NAME_CanvasPI ".surf");
    Surface  *surface = spawner.Alloc(ppvObj);
    if (surface)
    {
      surface->m_surf_def.Copy(&m_surf_def);
      return LXe_OK;
    }
    
    return LXe_FAILED;
  }

  int SurfInst::instable_Compare(ILxUnknownID other_obj)
  {
    /*
      The compare function is used to compare two instanceable objects. It's
      identical to strcmp and should either return 0 for identical, or -1/1
      to indicate relative position.
    */

    CLxSpawner<SurfInst>  spawner(SERVER_NAME_CanvasPI ".instObj");
    SurfInst             *other = spawner.Cast(other_obj);
    
    if (other)  return m_surf_def.Compare(&other->m_surf_def);
    else        return 0;
  }

  /*
    Implement the Package Instance.
  */

  class Instance : public CLxImpl_PackageInstance,
                   public CLxImpl_SurfaceItem,
                   public CLxImpl_ViewItem3D
  {
   public:
    static void initialize ()
    {
      CLxGenericPolymorph *srv = NULL;
      srv = new CLxPolymorph                              <Instance>;
      srv->AddInterface       (new CLxIfc_PackageInstance <Instance>);
      srv->AddInterface       (new CLxIfc_SurfaceItem     <Instance>);
      srv->AddInterface       (new CLxIfc_ViewItem3D      <Instance>);
      lx::AddSpawner          (SERVER_NAME_CanvasPI ".inst", srv);
    }
    
    Instance()
    {
      feLog("CanvasPI::Instance::Instance() new BaseInterface");
      // init members and create base interface.
      m_item_obj = NULL;
      m_userData.zero();
      m_userData.baseInterface = new BaseInterface();
    }
    ~Instance()
    {
      // note: for some reason this destructor doesn't get called,
      //       so as a workaround the cleaning up, i.e. deleting the
      //       base interface, is done in the function pins_Cleanup().
    };

    LxResult    pins_Initialize(ILxUnknownID item_obj, ILxUnknownID super)  LXx_OVERRIDE;
    LxResult    pins_Newborn(ILxUnknownID original, unsigned flags)         LXx_OVERRIDE  { return ItemCommon::pins_Newborn(original, flags, m_item_obj, m_userData.baseInterface); }
    LxResult    pins_AfterLoad(void)                                        LXx_OVERRIDE  { return ItemCommon::pins_AfterLoad(m_item_obj, m_userData.baseInterface); }
    void        pins_Doomed(void)                                           LXx_OVERRIDE  { ItemCommon::pins_Doomed(m_userData.baseInterface); }
    void        pins_Cleanup(void)                                          LXx_OVERRIDE  { m_userData.clear(); }
    
    LxResult    isurf_GetSurface (ILxUnknownID chanRead_obj, unsigned morph, void **ppvObj) LXx_OVERRIDE;
    LxResult    isurf_Prepare    (ILxUnknownID eval_obj, unsigned *index)                   LXx_OVERRIDE;
    LxResult    isurf_Evaluate   (ILxUnknownID attr_obj, unsigned index, void **ppvObj)     LXx_OVERRIDE;

   public:
    ILxUnknownID        m_item_obj;
    piUserData          m_userData;
  };

  LxResult Instance::pins_Initialize(ILxUnknownID item_obj, ILxUnknownID super)
  {
    // store item ID in our member.
    m_item_obj = item_obj;

    //
    if (m_userData.baseInterface)   m_userData.baseInterface->m_ILxUnknownID_CanvasPI = item_obj;
    else                            feLogError("m_userData.baseInterface == NULL");

    // done.
    return LXe_OK;
  }

  LxResult Instance::isurf_GetSurface(ILxUnknownID chanRead_obj, unsigned morph, void **ppvObj)
  {
    /*
     *  This function is used to allocate a surface for displaying in the GL
     *  viewport. We're given a channel read object and we're expected to
     *  read the channels needed to generate the surface and then return the
     *  spawned surface. We simply read the instanceable channel and call
     *  its GetSurface function.
     */

    CLxUser_Item item(m_item_obj);
    if (item.test())
    {
      CLxUser_ChannelRead     chan_read(chanRead_obj);
      CLxUser_ValueReference  val_ref;
      CLxLoc_Instanceable     instanceable;
      if (chan_read.Object(item, CHN_NAME_INSTOBJ, val_ref))
      {
        if (val_ref.Get(instanceable) && instanceable.test())
          return instanceable.GetSurface (ppvObj);
      }
    }

    return LXe_FAILED;
  }

  LxResult Instance::isurf_Prepare(ILxUnknownID eval_obj, unsigned *index)
  {
    /*
     *  This function is used to allocate the channels needed to evaluate a
     *  surface. We just add a single channel; the instanceable. This can be
     *  used to generate the surface.
     */

    CLxUser_Item item(m_item_obj);
    if (item.test())
    {
      CLxUser_Evaluation eval(eval_obj);
      index[0] = eval.AddChan(item, CHN_NAME_INSTOBJ, LXfECHAN_READ);
      return LXe_OK;
    }

    return LXe_FAILED;
  }

  LxResult Instance::isurf_Evaluate(ILxUnknownID attr_obj, unsigned index, void **ppvObj)
  {
    /*
     *  This function is used to generate a surface in an evaluated context.
     *  We have a single input channel to the modifier which is the instanceable,
     *  so we read the object and call the GetSurface function.
     */
    
    CLxUser_Attributes      attr(attr_obj);
    CLxUser_ValueReference  val_ref;
    CLxLoc_Instanceable     instanceable;

    if (attr.ObjectRO (index, val_ref))
    {
      if (val_ref.Get(instanceable) && instanceable.test())
        return instanceable.GetSurface(ppvObj);
    }
    
    return LXe_FAILED;
  }

  /*
    Implement the Package.
  */

  class Package : public CLxImpl_Package,
                  public CLxImpl_ChannelUI,
                  public CLxImpl_SceneItemListener
  {
   public:
    static void initialize()
    {
      CLxGenericPolymorph *srv = NULL;
      srv = new CLxPolymorph                                <Package>;
      srv->AddInterface       (new CLxIfc_Package           <Package>);
      srv->AddInterface       (new CLxIfc_StaticDesc        <Package>);
      srv->AddInterface       (new CLxIfc_SceneItemListener <Package>);
      srv->AddInterface       (new CLxIfc_ChannelUI         <Package>);
      lx::AddServer           (SERVER_NAME_CanvasPI, srv);
    }

    Package () : m_inst_spawn(SERVER_NAME_CanvasPI ".inst") {}
    
    LxResult    pkg_SetupChannels   (ILxUnknownID addChan_obj)  LXx_OVERRIDE  { return ItemCommon::pkg_SetupChannels(addChan_obj, true); }
    LxResult    pkg_Attach          (void **ppvObj)             LXx_OVERRIDE  { m_inst_spawn.Alloc(ppvObj); return (ppvObj[0] ? LXe_OK : LXe_FAILED); }
    LxResult    pkg_TestInterface   (const LXtGUID *guid)       LXx_OVERRIDE  { return m_inst_spawn.TestInterfaceRC(guid); }

    LxResult    cui_UIHints         (const char *channelName, ILxUnknownID hints_obj)   LXx_OVERRIDE  { return ItemCommon::cui_UIHints(channelName, hints_obj); }

    void        sil_ItemAddChannel  (ILxUnknownID item_obj)                             LXx_OVERRIDE;
    void        sil_ItemChannelName (ILxUnknownID item_obj, unsigned int index)         LXx_OVERRIDE;

    static LXtTagInfoDesc descInfo[];
    
   private:
    CLxSpawner <Instance> m_inst_spawn;
  };

  void Package::sil_ItemAddChannel(ILxUnknownID item_obj)
  {
    /*
      When user channels are added to our item type, this function will be
      called. We use it to invalidate our modifier so that it's reallocated.
      We don't need to worry about channels being removed, as the evaluation
      system will automatically invalidate the modifier when channels it
      is accessing are removed.
     
      NOTE: This won't invalidate other modifiers that have called Prepare
      on our SurfaceItem directly.
    */
    
    CLxUser_Item  item(item_obj);
    CLxUser_Scene scene;
    
    if (item.test() && item.IsA(gItemType_CanvasPI.Type()))
    {
      if (item.GetContext(scene))
        scene.EvalModInvalidate(SERVER_NAME_CanvasPI ".mod");
    }
  }

  void Package::sil_ItemChannelName(ILxUnknownID item_obj, unsigned int index)
  {
    /*
      When a user channel's name changes, this function will be
      called. We use it to invalidate our modifier so that it's reallocated.
    */

    CLxUser_Item    item(item_obj);
    CLxUser_Scene   scene;

    if (item.test() && item.IsA(gItemType_CanvasPI.Type()))
    {
      if (item.GetContext(scene))
        scene.EvalModInvalidate(SERVER_NAME_CanvasPI ".mod");
    }
  }

  LXtTagInfoDesc Package::descInfo[] =
  {
    { LXsPKG_SUPERTYPE, LXsITYPE_LOCATOR },
    { LXsPKG_IS_MASK, "." },
    { LXsPKG_INSTANCEABLE_CHANNEL, CHN_NAME_INSTOBJ },
    { LXsSRV_LOGSUBSYSTEM, LOG_SYSTEM_NAME },
    { 0 }
  };

  /*
    Implement the Modifier Element and Server. This reads the input channels as
    read only channels and output channels as write only channels. It's purpose
    is to evaluate the surface definition input channels and output an
    instanceable COM object that represents the current state of the surface.
  */

  class Element : public CLxItemModifierElement
  {
   public:
    Element     (CLxUser_Evaluation &eval, ILxUnknownID item_obj);
    bool    Test(ILxUnknownID item_obj)                               LXx_OVERRIDE  { return ItemCommon::Test(item_obj, m_surf_def.m_userData->usrChan); }
    void    Eval(CLxUser_Evaluation &eval, CLxUser_Attributes &attr)  LXx_OVERRIDE;
    
   private:
    int                                 m_eval_index_InstObj;
    SurfDef                             m_surf_def;
  };

  Element::Element(CLxUser_Evaluation &eval, ILxUnknownID item_obj)
  {
    /*
      In the constructor, we want to add the input and output channels
      required for this modifier. The output is hardcoded as the instanceable
      object channel, but for the inputs, we fall through to the Surface
      Definition and let that define any channels it needs.
    */

    CLxUser_Item  item(item_obj);
    if (!item.test())
      return;

    // the first channel we add is the instanceable object channel as an output.
    m_eval_index_InstObj = eval.AddChan(item, CHN_NAME_INSTOBJ, LXfECHAN_WRITE);
    
    // call the prepare function on the surface definition to add the channels it needs.
    unsigned int temp = 0;
    m_surf_def.Prepare(eval, item, &temp);
  }

  void Element::Eval(CLxUser_Evaluation &eval, CLxUser_Attributes &attr)
  {
    /*
      The Eval function for the modifier reads input channels and writes
      output channels. We allocate an instanceable object and copy the
      surface definition to it - then we evaluate it's channels.
    */

    if (!eval || !attr)
      return;
    
    /*
      Spawn the instanceable object to store in the output channel. We
      get the output channel as a writeable Value Reference and then set
      the object it contains to our spawned instanceable object.
    */
    
    CLxSpawner <SurfInst> spawner (SERVER_NAME_CanvasPI ".instObj");
    ILxUnknownID object = NULL;
    SurfInst *instObj = spawner.Alloc(object);

    CLxUser_ValueReference val_ref;
    unsigned int temp_chan_index = m_eval_index_InstObj;
    if (instObj && attr.ObjectRW(temp_chan_index++, val_ref))
    {
      val_ref.SetObject(object);
    
      /*
        Copy the cached surface definition to the surface definition
        on the instanceable object.
      */
      instObj->m_surf_def.Copy(&m_surf_def);
            
      /*
        Call Evaluate on the Surface Defintion to get the
        channels required for evaluation.
      */
      instObj->m_surf_def.Evaluate(attr, temp_chan_index);
    }
  }

  class Modifier : public CLxItemModifierServer
  {
   public:
    static void initialize()  { CLxExport_ItemModifierServer <Modifier> (SERVER_NAME_CanvasPI ".mod"); }
    const char *ItemType()  LXx_OVERRIDE  { return SERVER_NAME_CanvasPI; }
    CLxItemModifierElement *Alloc(CLxUser_Evaluation &eval, ILxUnknownID item_obj)   LXx_OVERRIDE { return new Element (eval, item_obj); }
  };

  Instance *GetInstance(ILxUnknownID item_obj)
  {
    CLxUser_Item item(item_obj);
    if (!item.test())
      return NULL;

    // check the type.
    {
      const char *typeName = NULL;
      CLxUser_SceneService srv;
      if (srv.ItemTypeName(item.Type(), &typeName) != LXe_OK || !typeName)
        return NULL;

      if (strcmp(typeName, SERVER_NAME_CanvasPI))
        return NULL;
    }

    // get/return pointer at Instance.
    CLxLoc_PackageInstance pkg_inst(item_obj);
    if (pkg_inst.test())
    {
      CLxSpawner <Instance> spawn(SERVER_NAME_CanvasPI ".inst");
      return spawn.Cast(pkg_inst);
    }
    return NULL;
  }

  piUserData *GetInstanceUserData(ILxUnknownID item_obj)
  {
    Instance *inst = GetInstance(item_obj);
    if (inst)   return &inst->m_userData;
    else        return NULL;
  }

  BaseInterface *GetBaseInterface(ILxUnknownID item_obj)
  {
    Instance *inst = GetInstance(item_obj);
    if (inst)   return inst->m_userData.baseInterface;
    else        return NULL;
  }


  // used in the plugin's initialize() function (see plugin.cpp).
  void initialize()
  {
    Instance    :: initialize();
    Package     :: initialize();
    Modifier    :: initialize();
    SurfInst    :: initialize();
    Surface     :: initialize();
    SurfElement :: initialize();
  }
};  // namespace CanvasPI



