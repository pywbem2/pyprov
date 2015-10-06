/*****************************************************************************
* (C) Copyright 2007 Novell, Inc. 
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as
* published by the Free Software Foundation; either version 2 of the
* License, or (at your option) any later version.
*   
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*   
* You should have received a copy of the GNU Lesser General Public
* License along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*****************************************************************************/
#ifndef PYASSOCIATORPROVIDERHANDLDER_H_GUARD
#define PYASSOCIATORPROVIDERHANDLDER_H_GUARD

#include "PythonProviderManager.h"

PEGASUS_USING_PEGASUS;

namespace PythonProvIFC
{

class AssociatorProviderHandler
{
public:
    static CIMResponseMessage* handleAssociatorsRequest(
		CIMRequestMessage* message, 
		PyProviderRef& provref,
		PythonProviderManager* pgmr);

    static CIMResponseMessage* handleAssociatorNamesRequest(
		CIMRequestMessage* message, 
		PyProviderRef& provref,
		PythonProviderManager* pgmr);

    static CIMResponseMessage* handleReferencesRequest(
		CIMRequestMessage* message,
		PyProviderRef& provref,
		PythonProviderManager* pgmr);

    static CIMResponseMessage* handleReferenceNamesRequest(
		CIMRequestMessage* message,
		PyProviderRef& provref,
		PythonProviderManager* pgmr);
};

}	// end of namespace PythonProvIFC

#endif	//PYASSOCIATORPROVIDERHANDLDER_H_GUARD
