/* Hector -- A Simple Climate Model
   Copyright (C) 2014-2015  Battelle Memorial Institute

   Please see the accompanying file LICENSE.md for additional licensing
   information.
*/
/*
 *  ini_to_core_reader.cpp
 *  hector
 *
 *  Created by Pralit Patel on 11/1/10.
 *
 */

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>

#include "core/core.hpp"
#include "data/message_data.hpp"
#include "input/ini_to_core_reader.hpp"
#include "input/inih/ini.h"
#include "input/csv_table_reader.hpp"

namespace fs = boost::filesystem;

namespace Hector {
  
using namespace std;

//------------------------------------------------------------------------------
/*! \brief Constructor
 *
 *  Sets a pointer to the Core object which will handle routing read in data to
 *  the correct setData
 */
INIToCoreReader::INIToCoreReader( Core* core ):core( core )
{
}

//------------------------------------------------------------------------------
/*! \brief Destructor
 *  Does not delete core since its memory is managed elsewhere.
 */
INIToCoreReader::~INIToCoreReader() {
}

//------------------------------------------------------------------------------
/*! \brief Parse and INI file at the given name filename and route the data through
 *         the core.
 *  \param filename The INI file to be parsed by the core.
 *  \exception h_exception If either there was an error reading the INI file or
 *                         or there was a problem in the Core trying to set the
 *                         data.
 */
void INIToCoreReader::parse( const string& filename ) throw ( h_exception ) {
    iniFilePath = filename;
    int errorCode = ini_parse( filename.c_str(), valueHandler, this );
    
    // handle c errors by turning them into exceptions which can be handled later
    if( errorCode == -1 ) {
        H_THROW( "Could not open " + filename );
    } else if( errorCode != 0 ) {
        // throw the original exception caught by valueHandler
        throw valueHandlerException;
    }
}

//------------------------------------------------------------------------------
/*! \brief Private call back to bridge the c interface to the Core's interface.
 *
 *  Special cases to handle table input will be handled here and will keep the
 *  core in the dark in terms of how that data was generated.
 *
 *  \param user The instance of the parser who initiated the call back.
 *  \param section The INI section (component name, as interpreted by the core)
 *  \param name The name of the variable.
 *  \param value The value of the variable to set.
 *  \exception h_exception Any errors generated by the core while trying to setData.
 */
int INIToCoreReader::valueHandler( void* user, const char* section, const char* name,
                                  const char* value )
{
    static const string csvFilePrefix = "csv:";
    INIToCoreReader* reader = (INIToCoreReader*)user;
    
    H_ASSERT( reader->core, "core pointer is null!" );
    string nameStr = name;
    string valueStr = value;
    StringIter startBracket = find( nameStr.begin(), nameStr.end(), '[' );
    try {
        if( startBracket != nameStr.end() ) {
            // the variableName[2000] = 5.0 case
            StringIter endBracket = find( nameStr.begin(), nameStr.end(), ']' );
            
            // the parseTSeriesIndex method will do error checking on if the brackets
            // were found and make sense
            double valueIndex = parseTSeriesIndex( startBracket, endBracket, nameStr.end() );
            
            // substring the first part of name before the open bracket which is the
            // actual variable name the core knows about
            nameStr = string( static_cast<StringIter>( nameStr.begin() ), startBracket );
            message_data data( valueStr );
            data.date = valueIndex;
            reader->core->setData( section, nameStr, data );
        } else if( boost::starts_with( valueStr, csvFilePrefix ) ) {
            // the variableName = csv:input/table.csv case
            
            // remove the special case identifier to figure out the actual file name
            // to process
            string csvFileName( valueStr.begin() + csvFilePrefix.size(), valueStr.end() );
            // when not an absolute path consider the CSV filepath to be
            // relative to the INI file
            fs::path csvFilePath( csvFileName );
            if ( csvFilePath.is_relative() ) {
              fs::path iniFilePath( reader->iniFilePath );
              fs::path fullPath( iniFilePath.parent_path() / csvFilePath );
              csvFileName = fullPath.string();
            }

            CSVTableReader tableReader( csvFileName );
            tableReader.process( reader->core, section, nameStr );
        } else {
            // the typical variableName = value case
            // note that this implies name is not a time series variable and the
            // index will be left as the default uninitialized constant
            message_data data( valueStr );
            reader->core->setData( section, name, data );
        }
    }
    catch(const h_exception& e) {
        // We must guard this exception, since allowing it to
        // propogate back to ini_parse is unsafe.
        // Set the caught error so that when we get back from
        // ini_parse we will have the original exception to
        // rethrow
        reader->valueHandlerException = e;
        return 0;               // Error code for ini_parse
    }
    
    return 1;
}

//------------------------------------------------------------------------------
/*! \brief Parse a single time series index from a variable name.
 *
 *  The date index is assumed to be enclosed within square brackets.  Extra white
 *  space is allowed.
 *
 *  \param startBracket A string iterator pointing the the open bracket in the
 *                      variable name.
 *  \param endBracket A string iterator pointing the the close bracket in the
 *                    variable name.
 *  \param strEnd The end of the var name string used for error checking.
 *  \return The double time series index.
 *  \exception h_exception If the start or end brackets were invalid or if the
 *                         parsed index was not a double.
 */
double INIToCoreReader::parseTSeriesIndex( const StringIter startBracket,
                                          const StringIter endBracket,
                                          const StringIter strEnd )
{
    H_ASSERT( startBracket != strEnd, "index formatting issue" );
    H_ASSERT( endBracket != strEnd, "index formatting issue" );
    H_ASSERT( startBracket < endBracket, "index formatting issue" );
    
    using namespace boost;
    
    // substring out the string in between the brackets
    string dateIndexStr( startBracket + 1, endBracket );
    try {
        // remove extra white space before converting
        trim( dateIndexStr );
        return lexical_cast<double>( dateIndexStr );
    } catch( bad_lexical_cast& castException ) {
        H_THROW( "Could not convert index to double: "+dateIndexStr+", exception: "
                +castException.what() );
    }
}

}
