//
#ifndef SPLICE_MODO_PLUGIN
#define SPLICE_MODO_PLUGIN

// plugin version.
#define FABRICMODO_PLUGIN_VERSION	0.003

// disable some annoying VS warnings.
#pragma warning (disable : 4530)	// C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc.
#pragma warning (disable : 4800)	// forcing value to bool 'true' or 'false'.
#pragma warning (disable : 4806)	// unsafe operation: no value of type 'bool' promoted to type ...etc.

// includes.
#include "FabricUI/DFG/DFGWidget.h"
#include "class_FabricDFGWidget.h"
#include "class_BaseInterface.h"

// more includes.
#include <lx_chanmod.hpp>
#include <lx_item.hpp>
#include <lx_package.hpp>
#include "lx_plugin.hpp"
#include "lx_value.hpp"
#include "lxu_command.hpp"
#include "lxu_log.hpp"
#include "lxlog.h"

// log system (2/2).
#define	LOG_SYSTEM_NAME	"Fabric"
class CItemLog : public CLxLogMessage
{
	public:
	CItemLog() : CLxLogMessage(LOG_SYSTEM_NAME)	{	}
	const char *GetFormat()		{	return "n.a.";	}
 	const char *GetVersion()	{	return "n.a.";	};
 	const char *GetCopyright()	{	return "n.a.";	};
};
extern CItemLog gLog;
void feLog(void *userData, const char *s, unsigned int length);
void feLogError(void *userData, const char *s, unsigned int length);

#endif

// end of file.


