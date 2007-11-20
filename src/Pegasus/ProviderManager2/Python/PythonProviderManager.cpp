#include "PG_PyProvIFCCommon.h"
#include "PythonProviderManager.h"

#include "PyInstanceProviderHandler.h"
#include "PyMethodProviderHandler.h"
#include "PyAssociatorProviderHandler.h"
#include "PyIndicationProviderHandler.h"
#include "PyIndConsumerProviderHandler.h"

#include <Pegasus/Common/CIMMessage.h>
#include <Pegasus/Common/OperationContext.h>
#include <Pegasus/Common/OperationContextInternal.h>
#include <Pegasus/Common/Tracer.h>
#include <Pegasus/Common/StatisticalData.h>
#include <Pegasus/Common/Logger.h>
#include <Pegasus/Common/LanguageParser.h>
#include <Pegasus/Common/MessageLoader.h> //l10n
#include <Pegasus/Common/Constants.h>
#include <Pegasus/Common/FileSystem.h>
#include <Pegasus/Common/Mutex.h>
#include <Pegasus/Config/ConfigManager.h>
#include <Pegasus/Provider/CIMOMHandleQueryContext.h>
#include <Pegasus/ProviderManager2/CIMOMHandleContext.h>
#include <Pegasus/ProviderManager2/ProviderName.h>
#include <Pegasus/ProviderManager2/AutoPThreadSecurity.h>

#include "PG_PyConverter.h"

PEGASUS_USING_STD;
PEGASUS_USING_PEGASUS;

namespace PythonProvIFC
{

namespace
{

Py::Object g_cimexobj;
Mutex g_provGuard;

}	// End of unnamed namespace

//////////////////////////////////////////////////////////////////////////////
bool
strEndsWith(const String& src, const String& tok)
{
	bool rc = false;
	Uint32 ndx = src.find(tok);
	return (ndx == PEG_NOT_FOUND) ? false : (ndx == (src.size()-tok.size()));
}

//////////////////////////////////////////////////////////////////////////////
String
getPyFile(const String& fname)
{
	String pyFile;
	if (strEndsWith(fname, ".pyc"))
	{
		pyFile = fname.subString(0, fname.size()-1);
	}
	else if (strEndsWith(fname, ".py"))
	{
		pyFile = fname;
	}
	else
	{
		pyFile = fname + ".py";
	}
	return pyFile;
}

//////////////////////////////////////////////////////////////////////////////
Py::Object
getPyPropertyList(const CIMPropertyList& pgpropList)
{
	if (!pgpropList.isNull())
	{
		Py::List plst;
		Uint32 len = pgpropList.size();
		for(Uint32 i = 0; i < len; i++)
		{
			plst.append(Py::String(pgpropList[i].getString()));
		}
		return Py::Object(plst);
	}
	return Py::None();
}

//////////////////////////////////////////////////////////////////////////////
time_t
getModTime(const String& fileName)
{
	time_t cc = 0;
	struct stat stbuf;
	if (::stat((const char *)fileName.getCString(), &stbuf) == 0)
	{
		cc = stbuf.st_mtime;
	}
	return cc;
}

//////////////////////////////////////////////////////////////////////////////
String
getFunctionName(
	const String& fnameArg)
{
	return String(PYFUNC_PREFIX + fnameArg);
}

//////////////////////////////////////////////////////////////////////////////
Py::Callable
getFunction(
	const Py::Object& obj,
	const String& fnameArg)
{
	String fn = getFunctionName(fnameArg);
	try
	{
		Py::Callable pyfunc = obj.getAttr(fn);
		return pyfunc;
	}
	catch(Py::Exception& e)
	{
		e.clear();
	}
	THROW_NOSUCHMETH_EXC(fn);
	return Py::Callable();	// Shouldn't hit this
}

//////////////////////////////////////////////////////////////////////////////
String 
processPyException(
	Py::Exception& thrownEx,
	int lineno, 
	const String& provPath, 
	OperationResponseHandler* pHandler)
{
	Py::Object etype, evalue;

	bool isCIMExc = PyErr_ExceptionMatches(g_cimexobj.ptr());
	String tb = LogPyException(thrownEx, __FILE__, lineno, etype,
		evalue, !isCIMExc);

	thrownEx.clear();

	if (!pHandler)
	{
		return tb;
	}

	if (!isCIMExc)
	{
		pHandler->setCIMException(CIMException(CIM_ERR_FAILED,
			Formatter::format("File: $0  Line: $1  From Python code. "
				"Trace: $2", __FILE__, lineno, tb)));
		return tb;
	}

	int errval = CIM_ERR_FAILED;
	String msg = Formatter::format("Thrown from Python provider: $0", provPath);
	try
	{
		// Attempt to get information about the pywbem.CIMError
		// that occurred...
		bool haveInt = false;
		bool haveMsg = false;
		Py::Tuple exargs = evalue.getAttr("args");
		for (int i = 0; i < int(exargs.length()); i++)
		{
			Py::Object wko = exargs[i];
			if (wko.isInt() && !haveInt)
			{
				errval = int(Py::Int(wko));
				haveInt = true;
			}
			if (wko.isString() && !haveMsg)
			{
				msg = Py::String(wko).as_peg_string();
				haveMsg = true;
			}
			if (haveInt && haveMsg)
			{
				break;
			}
		}
	}
	catch(Py::Exception& theExc)
	{
		// TESTING
		Py::Object etype1, evalue1;
		bool isCIMExc = PyErr_ExceptionMatches(g_cimexobj.ptr());
		String tb1 = LogPyException(thrownEx, __FILE__, lineno, etype1,
			evalue1, false);
		theExc.clear();
		pHandler->setCIMException(CIMException(CIM_ERR_FAILED,
			Formatter::format("Re-Thrown from python code. type: $0  value: $1",
				etype.as_string(), evalue.as_string())));
		return tb;
	}
	catch(...)
	{
		pHandler->setCIMException(CIMException(CIM_ERR_FAILED,
			Formatter::format("Caught unknown exception trying to process "
				"pywbem.CIMError. type: $0  value: $1",
				etype.as_string(), evalue.as_string())));
		return tb;
	}

	pHandler->setCIMException(CIMException(CIMStatusCode(errval),
		Formatter::format("$0 File: $1  Line: $2",
			msg, __FILE__, lineno)));
	return tb;
}

///////////////////////////////////////////////////////////////////////////////
PythonProviderManager::PythonProviderManager()
	: ProviderManager()
	, m_pywbemMod()
	, m_pPyExtensions(0)
	, m_provs()
	, m_mainPyThreadState(0)
{
	cerr << "*****In PythonProviderManager::ctor" << endl;
    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::PythonProviderManager()");
    _subscriptionInitComplete = false;
    PEG_TRACE_CSTRING (
        TRC_PROVIDERMANAGER,
        Tracer::LEVEL2,
        "-- Python Provider Manager activated");

	_initPython();

    PEG_METHOD_EXIT();
}

///////////////////////////////////////////////////////////////////////////////
PythonProviderManager::~PythonProviderManager()
{
	cerr << "*****In PythonProviderManager::~dtor" << endl;
    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::~PythonProviderManager()");
	PyEval_AcquireLock();
	PyThreadState_Swap(m_mainPyThreadState);
	Py_Finalize();
    PEG_METHOD_EXIT();
}

///////////////////////////////////////////////////////////////////////////////
Py::Object
PythonProviderManager::_loadProvider(
	const String& provPath,
	const OperationContext& opctx)
{
	try
	{
		Py::Object cim_provider = m_pywbemMod.getAttr("cim_provider"); 
		Py::Callable ctor = cim_provider.getAttr("ProviderProxy");
		Py::Tuple args(2);
		args[0] = PyProviderEnvironment::newObject(opctx); 	// Provider Environment
		args[1] = Py::String(provPath);
		// Construct a CIMProvider python object
		Py::Object pyprov = ctor.apply(args);
		return pyprov;
	}
	catch(Py::Exception& e)
	{
		Logger::put(Logger::ERROR_LOG, PYSYSTEM_ID, Logger::SEVERE,
			"ProviderManager.Python.PythonProviderManager",
			"Caught exception loading provider $0.",
			provPath);
		String tb = processPyException(e, __LINE__, provPath, false);
		String msg = "Python Load Error: " + tb;
		THROW_NOSUCHPROV_EXC(msg);
	}
	return Py::None();
}


///////////////////////////////////////////////////////////////////////////////
void
PythonProviderManager::_shutdownProvider(
	const PyProviderRep& provrep,
	const OperationContext& opctx)
{
	Py::GILGuard gg;	// Acquire python's GIL
	try
	{
cerr << "******* about to call 'shutdown' (for reload?  or for shutdown?) *****" << endl;
		Py::Callable pyfunc = getFunction(provrep.m_pyprov, "shutdown");
		Py::Tuple args(1);
		args[0] = PyProviderEnvironment::newObject(opctx); 	// Provider Environment
	    pyfunc.apply(args);
cerr << "******* Done with 'shutdown' and cleanup *****" << endl;
	}
	catch(Py::Exception& e)
	{
cerr << "******* Got exception unloading provider *****" << endl;
		Logger::put(Logger::ERROR_LOG, PYSYSTEM_ID, Logger::SEVERE,
			"ProviderManager.Python.PythonProviderManager",
			"Caught python exception invoking 'shutdown' provider $0.",
			provrep.m_path);
		String tb = processPyException(e, __LINE__, provrep.m_path);
		String msg = "Python Unload Error: " + tb;
//cerr << (const char *)msg.getCString() << endl;
cerr << msg << endl;
	}
	catch(...)
	{
cerr << "******* Got unknown exception unloading provider *****" << endl;
		Logger::put(Logger::ERROR_LOG, PYSYSTEM_ID, Logger::SEVERE,
			"ProviderManager.Python.PythonProviderManager",
			"Caught unknown exception invoking 'shutdown' provider $0.",
			provrep.m_path);
		String msg = "Python Unload Error: Unknown error";
cerr << msg << endl;
	}
}

///////////////////////////////////////////////////////////////////////////////
void
PythonProviderManager::_incActivationCount(
	PyProviderRep& provrep)
{
	AutoMutex am(g_provGuard);
	ProviderMap::iterator it = m_provs.find(provrep.m_path);
	if (it != m_provs.end())
	{
		it->second.m_activationCount++;
		provrep.m_activationCount = it->second.m_activationCount;
	}
}

///////////////////////////////////////////////////////////////////////////////
void
PythonProviderManager::_decActivationCount(
	PyProviderRep& provrep)
{
	AutoMutex am(g_provGuard);
	ProviderMap::iterator it = m_provs.find(provrep.m_path);
	if (it != m_provs.end())
	{
		it->second.m_activationCount--;
		provrep.m_activationCount = it->second.m_activationCount;
	}
}

///////////////////////////////////////////////////////////////////////////////
PyProviderRep
PythonProviderManager::_path2PyProviderRep(
	const String& provPath,
	const OperationContext& opctx)
{
	AutoMutex am(g_provGuard);

cerr << "*** in _path2PyProviderRep" << endl;
	ProviderMap::iterator it = m_provs.find(provPath);
	if (it != m_provs.end())
	{
		it->second.m_lastAccessTime = ::time(NULL);
        time_t curModTime = getModTime(provPath);
        if ((curModTime <= it->second.m_fileModTime)
            || (it->second.m_canUnload == false) )
        {
cerr << "*** _path2PyProviderRep returning 1" << endl;
            // not modified, or can't reload... return it
            return it->second;
        }
        else
        {
            //cleanup for reload on fall-thru
            _shutdownProvider(it->second, opctx);
            m_provs.erase(it);
        }
	}

	Py::GILGuard gg;	// Acquire python's GIL

	try
	{
		// Get the Python proxy provider
		Py::Object pyprov = _loadProvider(provPath, opctx);
		PyProviderRep entry(provPath, pyprov);
		entry.m_fileModTime = getModTime(provPath);
		entry.m_lastAccessTime = ::time(NULL);
		m_provs[provPath] = entry;
cerr << "*** _path2PyProviderRep returning 2" << endl;
		return entry;
	}
	catch(Py::Exception& e)
	{
cerr << "*** _path2PyProviderRep caught Py::exception" << endl;
		Logger::put(Logger::ERROR_LOG, PYSYSTEM_ID, Logger::SEVERE,
			"ProviderManager.Python.PythonProviderManager",
			"Caught exception loading provider $0.",
			provPath);
		String tb = processPyException(e, __LINE__, provPath);
		String msg = "Python Load Error: " + tb;
		THROW_NOSUCHPROV_EXC(msg);
	}

	// Shouldn't hit this
	return PyProviderRep();
}

///////////////////////////////////////////////////////////////////////////////
Message*
PythonProviderManager::processMessage(Message * message)
{
	cerr << "*****In PythonProviderManager::processMessage" << endl;
    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::processMessage()");

    CIMRequestMessage* request = dynamic_cast<CIMRequestMessage*>(message);
    PEGASUS_ASSERT(request != 0);

	ProviderIdContainer providerId =
		request->operationContext.get(ProviderIdContainer::NAME);

	ProviderName name = _resolveProviderName(providerId);

	// If provider doesn't exist this call throws PyNoSuchProviderException
	PyProviderRep provRep = _path2PyProviderRep(name.getLocation(),
		request->operationContext);

	// At this point we know we have a provider
	CIMResponseMessage* response = 0;

	// pass the request message to a handler method based on message type
	switch (request->getType())
	{
		case CIM_GET_INSTANCE_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleGetInstanceRequest(request, provRep, this);
			break;

		case CIM_ENUMERATE_INSTANCES_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleEnumerateInstancesRequest(request, provRep, this);
			break;

		case CIM_ENUMERATE_INSTANCE_NAMES_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleEnumerateInstanceNamesRequest(request, provRep, this);
			break;

		case CIM_CREATE_INSTANCE_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleCreateInstanceRequest(request, provRep, this);
			break;

		case CIM_MODIFY_INSTANCE_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleModifyInstanceRequest(request, provRep, this);
			break;

		case CIM_DELETE_INSTANCE_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleDeleteInstanceRequest(request, provRep, this);
			break;

		case CIM_GET_PROPERTY_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleGetPropertyRequest(request, provRep, this);
			break;

		case CIM_SET_PROPERTY_REQUEST_MESSAGE:
			response = InstanceProviderHandler::handleSetPropertyRequest(request, provRep, this);
			break;

		case CIM_INVOKE_METHOD_REQUEST_MESSAGE:
			response = MethodProviderHandler::handleInvokeMethodRequest(request, provRep, this);
			break;

		case CIM_ASSOCIATORS_REQUEST_MESSAGE:
			response = AssociatorProviderHandler::handleAssociatorsRequest(request, provRep, this);
			break;

		case CIM_ASSOCIATOR_NAMES_REQUEST_MESSAGE:
			response = AssociatorProviderHandler::handleAssociatorNamesRequest(request, provRep, this);
			break;

		case CIM_REFERENCES_REQUEST_MESSAGE:
			response = AssociatorProviderHandler::handleReferencesRequest(request, provRep, this);
			break;
		case CIM_REFERENCE_NAMES_REQUEST_MESSAGE:
			cerr << "***** ProviderManager calling handleReferenceNamesRequest" << endl;
			response = AssociatorProviderHandler::handleReferenceNamesRequest(request, provRep, this);
			cerr << "***** ProviderManager handleReferenceNamesRequest returned" << endl;
			break;

		case CIM_EXEC_QUERY_REQUEST_MESSAGE:
			// TODO
			response = _handleExecQueryRequest(request, provRep);
			break;

		case CIM_CREATE_SUBSCRIPTION_REQUEST_MESSAGE:
			cerr << "***** CIM_CREATE_SUBSCRIPTION_REQUEST_MESSAGE came through" << endl;
			_incActivationCount(provRep);
			response = IndicationProviderHandler::handleCreateSubscriptionRequest(request, provRep, this);
			break;

		case CIM_DELETE_SUBSCRIPTION_REQUEST_MESSAGE:
			cerr << "***** CIM_DELETE_SUBSCRIPTION_REQUEST_MESSAGE came through" << endl;
			_decActivationCount(provRep);
			response = IndicationProviderHandler::handleDeleteSubscriptionRequest(request, provRep, this);
			break;

		case CIM_MODIFY_SUBSCRIPTION_REQUEST_MESSAGE:
			cerr << "***** CIM_MODIFY_SUBSCRIPTION_REQUEST_MESSAGE came through" << endl;
			//response = IndicationProviderHandler::handleModifySubscriptionRequest(request, provRep, this);
			break;

		case CIM_EXPORT_INDICATION_REQUEST_MESSAGE:
			cerr << "***** CIM_EXPORT_INDICATION_REQUEST_MESSAGE came through" << endl;

			response = IndicationConsumerProviderHandler::handleExportIndicationRequest(
				request, provRep, this);
			break;

		case CIM_DISABLE_MODULE_REQUEST_MESSAGE:
			cerr << "***** CIM_DISABLE_MODULE_REQUEST_MESSAGE came through" << endl;
			// TODO
			response = _handleDisableModuleRequest(request, provRep);
			break;

		case CIM_ENABLE_MODULE_REQUEST_MESSAGE:
			cerr << "***** CIM_ENABLE_MODULE_REQUEST_MESSAGE came through" << endl;
			// TODO
			response = _handleEnableModuleRequest(request, provRep);
			break;

		case CIM_STOP_ALL_PROVIDERS_REQUEST_MESSAGE:
			cerr << "***** CIM_STOP_ALL_PROVIDERS_REQUEST_MESSAGE came through" << endl;
			// TODO
			response = _handleStopAllProvidersRequest(request, provRep);
			break;

// Note: The PG_Provider AutoStart property is not yet supported
#if 0
		case CIM_INITIALIZE_PROVIDER_REQUEST_MESSAGE:
			response = _handleInitializeProviderRequest(request, provRep);
			break;
#endif
		case CIM_SUBSCRIPTION_INIT_COMPLETE_REQUEST_MESSAGE:
			cerr << "***** CIM_SUBSCRIPTION_INIT_COMPLETE_REQUEST_MESSAGE came through" << endl;
			// TODO
			response = _handleSubscriptionInitCompleteRequest (request, provRep);
			break;

		default:
			response = _handleUnsupportedRequest(request, provRep);
			break;
	}
    PEG_METHOD_EXIT();
	cerr << "processMessage returning response" << endl;
    return(response);
}

///////////////////////////////////////////////////////////////////////////////
Boolean PythonProviderManager::hasActiveProviders()
{
	// TODO
	return false;
}

///////////////////////////////////////////////////////////////////////////////
void PythonProviderManager::unloadIdleProviders()
{
	// TODO
}

///////////////////////////////////////////////////////////////////////////////
CIMResponseMessage * PythonProviderManager::_handleExecQueryRequest(
	CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleExecQueryRequest()");

    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"ExecQuery not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleCreateSubscriptionRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleCreateSubscriptionRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"CreateSubscription not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleDeleteSubscriptionRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleDeleteSubscriptionRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"DeleteScription not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleDisableModuleRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleDisableModuleRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"DisableModuleRequest not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleEnableModuleRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleEnableModuleRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"EnableModuleRequest not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleStopAllProvidersRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleStopAllProvidersRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"StopAllProviders not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

#if 0
CIMResponseMessage * PythonProviderManager::_handleInitializeProviderRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleSubscriptionInitCompleteRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"SubscriptionInitComplete not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}
#endif

CIMResponseMessage * PythonProviderManager::_handleSubscriptionInitCompleteRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleSubscriptionInitCompleteRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED,
				"SubscriptionInitComplete not yet implemented");
    PEG_METHOD_EXIT();
    return response;
}

CIMResponseMessage * PythonProviderManager::_handleUnsupportedRequest(
    CIMRequestMessage * message,
	PyProviderRep& provrep)
{
	Py::GILGuard gg;	// Acquire Python's GIL

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_handleUnsupportedRequest()");
    CIMRequestMessage* request =
        dynamic_cast<CIMRequestMessage *>(message);
    PEGASUS_ASSERT(request != 0 );

    CIMResponseMessage* response = request->buildResponse();
    response->cimException =
        PEGASUS_CIM_EXCEPTION(CIM_ERR_NOT_SUPPORTED, String::EMPTY);

    PEG_METHOD_EXIT();
    return response;
}

String PythonProviderManager::_resolvePhysicalName(String physicalName)
{
	String defProvDir = "/usr/lib/pycim";
	String provDir;
	//String provDir = ConfigManager::getInstance()->getCurrentValue("pythonProvDir");
	//cerr << "cfgProvDir : " << provDir << endl;

	if (provDir == String::EMPTY)
		provDir = defProvDir;
	if (!strEndsWith(provDir, "/"))
		provDir.append("/");
	
	String pyFileName = getPyFile(physicalName);
	String fullPath;
  if (pyFileName[0] == '/')
	{
		fullPath = pyFileName;
	}
	else
	{
		fullPath = provDir + pyFileName;
	}
	
	return fullPath;
}

ProviderName PythonProviderManager::_resolveProviderName(
    const ProviderIdContainer & providerId)
{
    String providerName;
    String fileName;
    String location;
    String moduleName;
    CIMValue genericValue;

    PEG_METHOD_ENTER(
        TRC_PROVIDERMANAGER,
        "PythonProviderManager::_resolveProviderName()");

    genericValue = providerId.getModule().getProperty(
        providerId.getModule().findProperty("Name")).getValue();
    genericValue.get(moduleName);
cerr << "######## moduleName: " << moduleName << endl;

    genericValue = providerId.getProvider().getProperty(
        providerId.getProvider().findProperty("Name")).getValue();
    genericValue.get(providerName);
cerr << "######## providerName: " << providerName << endl;

    genericValue = providerId.getModule().getProperty(
        providerId.getModule().findProperty("Location")).getValue();
    genericValue.get(location);
cerr << "######## location: " << location << endl;
	cerr << "About to resolvePhysicalName for : " << location << endl;
    fileName = _resolvePhysicalName(location);
cerr << "######## fileName: " << fileName << endl;

    // An empty file name is only for interest if we are in the 
    // local name space. So the message is only issued if not
    // in the remote Name Space.
    if (fileName == String::EMPTY && (!providerId.isRemoteNameSpace()))
    {
        genericValue.get(location);
        //String fullName = FileSystem::buildLibraryFileName(location);
        String fullName = location;
        Logger::put(Logger::ERROR_LOG, PYSYSTEM_ID, Logger::SEVERE,
            "ProviderManager.Python.PythonProviderManager.CANNOT_FIND_MODULE",
            "For provider $0 module $1 was not found.", 
            providerName, fullName);

    }
    ProviderName name(moduleName, providerName, fileName);
    name.setLocation(fileName);
    PEG_METHOD_EXIT();
    return name;
}

//////////////////////////////////////////////////////////////////////////////
void PythonProviderManager::_initPython()
{
	cerr << "*****In PythonProviderManager::_initPython" << endl;
	PEG_METHOD_ENTER(
		TRC_PROVIDERMANAGER,
		"PythonProviderManager::_initPython()");

	// Initialize embedded python interpreter
	Py_Initialize();
	PyEval_InitThreads();
	m_mainPyThreadState = PyGILState_GetThisThreadState();
	PyEval_ReleaseThread(m_mainPyThreadState);

	Py::GILGuard gg;	// Acquire python's GIL

	try
	{
		// Explicitly importing 'threading' right here seems to get rid of the
		// atexit error we get when exiting after a provider has imported
		// threading. Might have something to do with importing threading
		// from the main thread...
		//Py::Module tmod = Py::Module("threading", true);	// Explicity import threading

		// Load the pywbem module for use in interacting
		// with python providers
		PEG_TRACE_CSTRING (
			TRC_PROVIDERMANAGER,
			Tracer::LEVEL2,
			"Python provider manager loading pywbem module...");

		m_pywbemMod = Py::Module("pywbem", true);	// load pywbem module
		g_cimexobj = m_pywbemMod.getAttr("CIMError");
		PGPyConv::setPyWbemMod(m_pywbemMod);
	}
	catch (Py::Exception& e)
	{
		//m_disabled = true;
		cerr << "*****In PythonProviderManager::_initPython... caught exception loading pywbem module" << endl;
		String msg = "Python provider manager caught exception "
			"loading pywbem module:";
		Logger::put(Logger::STANDARD_LOG, PYSYSTEM_ID, Logger::FATAL, msg);
		String tb = LogPyException(e, __FILE__, __LINE__);
		e.clear();
		msg.append(tb);
		THROW_PYIFC_EXC(msg);
	}

	try
	{
		// Initialize the pycimmb module (python provider support module)
		Logger::put(Logger::DEBUG_LOG, PYSYSTEM_ID, Logger::TRACE,
			"Python provider mananger initializing the pycimmb module...");
		PyExtensions::doInit(m_pywbemMod);
		m_pPyExtensions = PyExtensions::getModulePtr();
	}
	catch (Py::Exception& e)
	{
		String msg = "Python provider manager caught exception "
			"initializing pycim module:";
		Logger::put(Logger::STANDARD_LOG, PYSYSTEM_ID, Logger::FATAL, msg);
		String tb = LogPyException(e, __FILE__, __LINE__);
		e.clear();
		msg.append(tb);
		THROW_PYIFC_EXC(msg);
	}
}

}	// End of namespace PythonProvIFC

extern "C" PEGASUS_EXPORT ProviderManager * PegasusCreateProviderManager(
    const Pegasus::String & providerManagerName)
{
    if (Pegasus::String::equalNoCase(providerManagerName, "Python"))
    {
        return(new PythonProvIFC::PythonProviderManager());
    }
    return(0);
}
    