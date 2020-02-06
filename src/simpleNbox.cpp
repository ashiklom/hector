/* Hector -- A Simple Climate Model
   Copyright (C) 2014-2015  Battelle Memorial Institute

   Please see the accompanying file LICENSE.md for additional licensing
   information.
*/
/*
 *  SimpleNbox.cpp
 *  hector
 *
 *  Created by Ben on 5/9/11.
 *
 */

#include "boost/algorithm/string.hpp"

#include "dependency_finder.hpp"
#include "simpleNbox.hpp"
#include "avisitor.hpp"

#include <algorithm>
#include <math.h>

#ifdef USE_RCPP
#include <Rcpp.h>
#endif

namespace Hector {

using namespace boost;

//------------------------------------------------------------------------------
/*! \brief constructor
 */
SimpleNbox::SimpleNbox() : CarbonCycleModel( 7 ), masstot(0.0) {
    ffiEmissions.allowInterp( true );
    ffiEmissions.name = "ffiEmissions";
    lucEmissions.allowInterp( true );
    lucEmissions.name = "lucEmissions";
    Ftalbedo.allowInterp( true );
    Ftalbedo.name = "albedo";

    // earth_c keeps track of how much fossil C is pulled out
    // so that we can do a mass-balance check throughout the run
    earth_c.set( 0.0, U_PGC );

}

//------------------------------------------------------------------------------
// documentation is inherited
void SimpleNbox::init( Core* coreptr ) {
    CarbonCycleModel::init( coreptr );

    core = coreptr;

    // Defaults
    co2fert[ SNBOX_DEFAULT_BIOME ] = 1.0;
    warmingfactor[ SNBOX_DEFAULT_BIOME ] = 1.0;
    residual.set( 0.0, U_PGC );
    tempfertd[ SNBOX_DEFAULT_BIOME ] = 1.0;
    tempferts[ SNBOX_DEFAULT_BIOME ] = 1.0;
    f_frozen[ SNBOX_DEFAULT_BIOME ] = 1.0;
    new_thaw[ SNBOX_DEFAULT_BIOME ] = 0.0;

    rh_ch4_frac[ SNBOX_DEFAULT_BIOME ] = 0.0;

    // Initialize the `biome_list` with just "global"
    biome_list.push_back( SNBOX_DEFAULT_BIOME );

    Tgav_record.allowInterp( true );

    // Register the data we can provide
    core->registerCapability( D_ATMOSPHERIC_CO2, getComponentName() );
    core->registerCapability( D_ATMOSPHERIC_C, getComponentName() );
    core->registerCapability( D_PREINDUSTRIAL_CO2, getComponentName() );
    core->registerCapability( D_RF_T_ALBEDO, getComponentName() );
    core->registerCapability( D_LAND_CFLUX, getComponentName() );
    core->registerCapability( D_VEGC, getComponentName() );
    core->registerCapability( D_DETRITUSC, getComponentName() );
    core->registerCapability( D_SOILC, getComponentName() );
    core->registerCapability( D_PERMAFROSTC, getComponentName() );
    core->registerCapability( D_NPP_FLUX0, getComponentName() );
    core->registerCapability( D_NPP, getComponentName() );
    core->registerCapability( D_RH, getComponentName() );
    core->registerCapability( D_RH_DETRITUS, getComponentName() );
    core->registerCapability( D_RH_SOIL, getComponentName() );
    core->registerCapability( D_RH_CH4, getComponentName() );
    core->registerCapability( D_F_FROZEN, getComponentName() );

    // Register our dependencies
    core->registerDependency( D_OCEAN_CFLUX, getComponentName() );

    // Register the inputs we can receive from outside
    core->registerInput(D_FFI_EMISSIONS, getComponentName());
    core->registerInput(D_LUC_EMISSIONS, getComponentName());
    core->registerInput(D_PREINDUSTRIAL_CO2, getComponentName());
    core->registerInput(D_VEGC, getComponentName());
    core->registerInput(D_DETRITUSC, getComponentName());
    core->registerInput(D_SOILC, getComponentName());
    core->registerInput(D_PERMAFROSTC, getComponentName());
    core->registerInput(D_NPP_FLUX0, getComponentName());
    core->registerInput(D_WARMINGFACTOR, getComponentName());
    core->registerInput(D_BETA, getComponentName());
    core->registerInput(D_Q10_RH, getComponentName());
    core->registerInput(D_F_NPPV, getComponentName());
    core->registerInput(D_F_NPPD, getComponentName());
    core->registerInput(D_F_LITTERD, getComponentName());
    core->registerInput(D_F_LUCV, getComponentName());
    core->registerInput(D_F_LUCD, getComponentName());
    core->registerInput(D_RH_CH4_FRAC, getComponentName());
}

//------------------------------------------------------------------------------
// documentation is inherited
unitval SimpleNbox::sendMessage( const std::string& message,
                                const std::string& datum,
                                const message_data info ) throw ( h_exception )
{
    unitval returnval;

    if( message==M_GETDATA ) {          //! Caller is requesting data
        return getData( datum, info.date );

    } else if( message==M_SETDATA ) {   //! Caller is requesting to set data

        setData(datum, info);
        //TODO: change core so that parsing is routed through sendMessage
        //TODO: make setData private

    } else {                        //! We don't handle any other messages
        H_THROW( "Caller sent unknown message: "+message );
    }

    return returnval;
}

//------------------------------------------------------------------------------
// documentation is inherited
void SimpleNbox::setData( const std::string &varName,
                          const message_data& data ) throw( h_exception )
{
    // Does the varName contain our parse character? If so, split it
    std::vector<std::string> splitvec;
    boost::split( splitvec, varName, is_any_of( SNBOX_PARSECHAR ) );
    H_ASSERT( splitvec.size() < 3, "max of one separator allowed in variable names" );

    std::string biome = SNBOX_DEFAULT_BIOME;
    std::string varNameParsed = varName;
    auto it_global = std::find(biome_list.begin(), biome_list.end(), SNBOX_DEFAULT_BIOME);

    if( splitvec.size() == 2 ) {    // i.e., in form <biome>.<varname>
        biome = splitvec[ 0 ];
        varNameParsed = splitvec[ 1 ];
        if ( has_biome( SNBOX_DEFAULT_BIOME ) ) {
            H_LOG( logger, Logger::DEBUG ) << "Removing biome '" << SNBOX_DEFAULT_BIOME <<
                "' because you cannot have both 'global' and biome data. " << std::endl;
            // We don't use the `deleteBiome` function here because
            // when `setData` is used to initialize the core from the
            // INI file, most of the time series variables that
            // `deleteBiome` modifies have not been initialized yet.
            // This should be relatively safe because (1) we check
            // consistency of biome-specific variable sizes before
            // running, and (2) the R interface will not let you use
            // `setData` to modify the biome list.
            biome_list.erase( it_global );
        }
    }

    H_ASSERT( !(it_global != biome_list.end() && biome != SNBOX_DEFAULT_BIOME),
              "If one of the biomes is 'global', you cannot add other biomes." );

    // If the biome is not currently in the `biome_list`, and it's not
    // the "global" biome, add it to `biome_list`
    if ( biome != SNBOX_DEFAULT_BIOME && !has_biome( biome ) ) {
        H_LOG( logger, Logger::DEBUG ) << "Adding biome '" << biome << "' to `biome_list`." << std::endl;
        // We don't use `createBiome` here for the same reasons as above.
        biome_list.push_back( biome );
    }

    if (data.isVal) {
        H_LOG( logger, Logger::DEBUG ) << "Setting " << biome << "." << varNameParsed << "[" << data.date << "]=" << data.value_unitval << std::endl;
    }
    else {
        H_LOG( logger, Logger::DEBUG ) << "Setting " << biome << "." << varNameParsed << "[" << data.date << "]=" << data.value_str << std::endl;
    }
    try {
        // Initial pools
        if( varNameParsed == D_ATMOSPHERIC_C ) {
            // Hector input files specify initial atmospheric CO2 in terms of
            // the carbon pool, rather than the CO2 concentration.  Since we
            // don't have a place to store the initial carbon pool, we convert
            // it to initial concentration and store that.  It will be converted
            // back to carbon content when the state variables are set up in
            // prepareToRun.
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            H_ASSERT( biome == SNBOX_DEFAULT_BIOME, "atmospheric C must be global" );
            set_c0(data.getUnitval(U_PGC).value(U_PGC) * PGC_TO_PPMVCO2);
        }
        else if( varNameParsed == D_PREINDUSTRIAL_CO2 ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            H_ASSERT( biome == SNBOX_DEFAULT_BIOME, "preindustrial C must be global" );
            set_c0(data.getUnitval(U_PPMV_CO2).value(U_PPMV_CO2));
        }
        else if( varNameParsed == D_VEGC ) {
            // For `veg_c`, `detritus_c`, `soil_c`, and `permafrost_c`, if date
            // is not provided, set only the "current" model pool, without
            // touching the time series variable. This is to accommodate the way
            // the INI file is parsed. For interactive use, you will usually
            // want to pass the date -- otherwise, the current value will be
            // overridden by a `reset` (which includes code like `veg_c =
            // veg_c_tv.get(t)`).
            veg_c[ biome ] = data.getUnitval( U_PGC );
            if (data.date != Core::undefinedIndex()) {
                veg_c_tv.set(data.date, veg_c);
            }
        }
        else if( varNameParsed == D_DETRITUSC ) {
            detritus_c[ biome ] = data.getUnitval( U_PGC );
            if (data.date != Core::undefinedIndex()) {
                detritus_c_tv.set(data.date, detritus_c);
            }
        }
        else if( varNameParsed == D_SOILC ) {
            soil_c[ biome ] = data.getUnitval( U_PGC );
            if (data.date != Core::undefinedIndex()) {
                soil_c_tv.set(data.date, soil_c);
            }
        }
        else if( varNameParsed == D_PERMAFROSTC ){
            permafrost_c[ biome ] = data.getUnitval( U_PGC );
            if ( data.date != Core::undefinedIndex()) {
                // HACK: Reset the mass balance -- DWIM.
                unitval delta_c = permafrost_c_tv.get(data.date).at(biome) - data.getUnitval( U_PGC );
                earth_c = earth_c_ts.get(data.date) + delta_c;
                permafrost_c_tv.set(data.date, permafrost_c);
                earth_c_ts.set(data.date, earth_c);
            }
        }

        // Albedo effect
        else if( varNameParsed == D_RF_T_ALBEDO ) {
            H_ASSERT( data.date != Core::undefinedIndex(), "date required" );
            Ftalbedo.set( data.date, data.getUnitval( U_W_M2 ) );
        }

        // Partitioning
        else if( varNameParsed == D_F_NPPV ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            f_nppv[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_F_NPPD ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            f_nppd[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_F_LITTERD ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            f_litterd[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_F_LUCV ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            f_lucv = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_F_LUCD ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            f_lucd = data.getUnitval(U_UNITLESS);
        }

        // Initial fluxes
        else if( varNameParsed == D_NPP_FLUX0 ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            npp_flux0[ biome ] = data.getUnitval( U_PGC_YR );
        }

        // Fossil fuels and industry contributions--time series.  There are two
        // message versions for each of these: one for string data
        // read from an input file, and another for actual values
        // passed from another part of the program.
        else if( varNameParsed == D_FFI_EMISSIONS ) {
            H_ASSERT( data.date != Core::undefinedIndex(), "date required" );
            H_ASSERT( biome == SNBOX_DEFAULT_BIOME, "fossil fuels and industry emissions must be global" );
            ffiEmissions.set( data.date, data.getUnitval( U_PGC_YR ) );
        }
        else if( varNameParsed == D_LUC_EMISSIONS ) {
            H_ASSERT( data.date != Core::undefinedIndex(), "date required" );
            lucEmissions.set( data.date, data.getUnitval( U_PGC_YR ) );
        }
        // Atmospheric CO2 record to constrain model to (optional)
        else if( varNameParsed == D_CA_CONSTRAIN ) {
            H_ASSERT( data.date != Core::undefinedIndex(), "date required" );
            H_ASSERT( biome == SNBOX_DEFAULT_BIOME, "atmospheric constraint must be global" );
            Ca_constrain.set( data.date, data.getUnitval( U_PPMV_CO2 ) );
        }

        // Fertilization
        else if( varNameParsed == D_BETA ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            beta[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_WARMINGFACTOR ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            warmingfactor[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_Q10_RH ) {
            H_ASSERT( data.date == Core::undefinedIndex() , "date not allowed" );
            q10_rh[ biome ] = data.getUnitval(U_UNITLESS);
        }
        else if( varNameParsed == D_RH_CH4_FRAC ) {
           H_ASSERT( data.date == Core::undefinedIndex(), "date not allowed for RH CH4 fraction" );
           rh_ch4_frac[ biome ] = data.getUnitval( U_UNITLESS );
        }

        else {
            H_LOG( logger, Logger::DEBUG ) << "Unknown variable " << varName << std::endl;
            H_THROW( "Unknown variable name while parsing "+ getComponentName() + ": "
                    + varName );
        }
    } catch( h_exception& parseException ) {
        H_RETHROW( parseException, "Could not parse var: "+varName );
    }
}

//------------------------------------------------------------------------------
/*! \brief      Sanity checks
 *  \exception  If any of the sanity checks fails
 *
 *  This is called internally throughout the model run and performs sanity checks.
 *  For example, the main carbon pools (except earth) should always be positive;
 *  partitioning coefficients should not exceed 1; etc.
 */
void SimpleNbox::sanitychecks() throw( h_exception )
{

    // Make a few sanity checks here, and then return.
    H_ASSERT( atmos_c.value( U_PGC ) > 0.0, "atmos_c pool <=0" );

    for ( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        H_ASSERT( veg_c.at(biome).value( U_PGC ) >= 0.0, "veg_c pool < 0" );
        H_ASSERT( detritus_c.at(biome).value( U_PGC ) >= 0.0, "detritus_c pool < 0" );
        H_ASSERT( soil_c.at(biome).value( U_PGC ) >= 0.0, "soil_c pool < 0" );
        H_ASSERT( permafrost_c.at(biome).value( U_PGC ) >= 0.0, "permafrost_c pool < 0" );
        H_ASSERT( npp_flux0.at(biome).value( U_PGC_YR ) >= 0.0, "npp_flux0 < 0" );

        H_ASSERT( f_nppv.at(biome) >= 0.0, "f_nppv <0" );
        H_ASSERT( f_nppd.at(biome) >= 0.0, "f_nppd <0" );
        H_ASSERT( f_nppv.at(biome) + f_nppd.at(biome) <= 1.0, "f_nppv + f_nppd >1" );
        H_ASSERT( f_litterd.at(biome) >= 0.0 && f_litterd.at(biome) <= 1.0, "f_litterd <0 or >1" );
    }

    H_ASSERT( f_lucv >= 0.0, "f_lucv <0" );
    H_ASSERT( f_lucd >= 0.0, "f_lucd <0" );
    H_ASSERT( f_lucv + f_lucd <= 1.0, "f_lucv + f_lucd >1" );

    H_ASSERT( C0.value( U_PPMV_CO2 ) > 0.0, "C0 <= 0" );
    H_ASSERT( Ca.value( U_PPMV_CO2 ) > 0.0, "Ca <= 0" );
}

//------------------------------------------------------------------------------
/*! \brief      Sum a string->unitval map
 *  \param      pool to sum over
 *  \returns    Sum of the unitvals in the map
 *  \exception  If the map is empty
 */
unitval SimpleNbox::sum_map( unitval_stringmap pool ) const
{
    H_ASSERT( pool.size(), "can't sum an empty map" );
    unitval sum( 0.0, pool.begin()->second.units() );
    for( unitval_stringmap::const_iterator it = pool.begin(); it != pool.end(); it++ )
        sum = sum + it->second;
    return sum;
}

//------------------------------------------------------------------------------
/*! \brief      Sum a string->double map
 *  \param      pool to sum over
 *  \returns    Sum of the unitvals in the map
 *  \exception  If the map is empty
 */
double SimpleNbox::sum_map( double_stringmap pool ) const
{
    H_ASSERT( pool.size(), "can't sum an empty map" );
    double sum = 0.0;
    for( double_stringmap::const_iterator it = pool.begin(); it != pool.end(); it++ )
        sum = sum + it->second;
    return sum;
}

//------------------------------------------------------------------------------
/*! \brief      Log pool states
 *  \param      current date
 */
void SimpleNbox::log_pools( const double t )
{
    // Log pool states
    H_LOG( logger,Logger::DEBUG ) << "---- simpleNbox pool states at t=" << t << " ----" << std::endl;
    H_LOG( logger,Logger::DEBUG ) << "Atmos = " << atmos_c << std::endl;
    H_LOG( logger,Logger::DEBUG ) << "Biome \tveg_c \t\tdetritus_c \tsoil_c\t\t permafrost_c" << std::endl;
    for ( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        H_LOG( logger,Logger::DEBUG ) <<
            biome << "\t" <<
            veg_c[ biome ] << "\t" <<
            detritus_c[ biome ] << "\t\t" <<
            soil_c[ biome ] << "\t" <<
            permafrost_c[ biome ] <<
            std::endl;
    }
    H_LOG( logger,Logger::DEBUG ) << "Earth = " << earth_c << std::endl;
}

//------------------------------------------------------------------------------
// documentation is inherited
void SimpleNbox::prepareToRun() throw( h_exception )
{

    H_LOG( logger, Logger::DEBUG ) << "prepareToRun " << std::endl;

    // If any 'global' settings, there shouldn't also be regional
    if ( (has_biome( SNBOX_DEFAULT_BIOME )) & (biome_list.size() > 1) ) {
        H_THROW( "Cannot have both global and biome-specific data! "
                 "Did you forget to rename the default ('global') biome?")
    }

    // Ensure consistency between biome_list and all pools and fluxes
    H_ASSERT( biome_list.size() == veg_c.size(), "veg_c and biome_list data not same size" );
    H_ASSERT( biome_list.size() == detritus_c.size(), "detritus_c and biome_list not same size" );
    H_ASSERT( biome_list.size() == soil_c.size(), "soil_c and biome_list not same size" );
    H_ASSERT( biome_list.size() == permafrost_c.size(), "permafrost_c and biome_list not same size" );
    H_ASSERT( biome_list.size() == npp_flux0.size(), "npp_flux0 and biome_list not same size" );

    for ( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        H_LOG( logger, Logger::DEBUG ) << "Checking that data for biome '" << biome << "' is complete" << std::endl;
        H_ASSERT( detritus_c.count( biome ), "no biome data for detritus_c" );
        H_ASSERT( soil_c.count( biome ), "no biome data for soil_c" );
        H_ASSERT( permafrost_c.count( biome ), "no biome data for permafrost_c" );
        H_ASSERT( npp_flux0.count( biome ), "no biome data for npp_flux0" );

        H_ASSERT( beta.count( biome ), "no biome value for beta" );

        if ( !warmingfactor.count( biome )) {
            H_LOG( logger, Logger::NOTICE ) << "No warmingfactor set for biome '" << biome << "'. " <<
                "Setting to default value = 1.0" << std::endl;
            warmingfactor[ biome ] = 1.0;
        }

        if ( !rh_ch4_frac.count( biome )) {
            H_LOG( logger, Logger::NOTICE ) << "No RH CH4 fraction set for biome '" << biome << "'. " <<
                "Setting to default value = 0.0" << std::endl;
            rh_ch4_frac[ biome ] = 0.0;
        }

    }

    // Save a pointer to the ocean model in use
    omodel = dynamic_cast<CarbonCycleModel*>( core->getComponentByCapability( D_OCEAN_C ) );

    if( !Ftalbedo.size() ) {          // if no albedo data, assume constant
        unitval alb( -0.2, U_W_M2 ); // default is MAGICC value
        Ftalbedo.set( core->getStartDate(), alb );
        Ftalbedo.set( core->getEndDate(), alb );
    }

    double c0init = C0.value(U_PPMV_CO2);
    Ca.set(c0init, U_PPMV_CO2);
    atmos_c.set(c0init * PPMVCO2_TO_PGC, U_PGC);

    if( Ca_constrain.size() ) {
        Ca_constrain.allowPartialInterp( true );
        Logger& glog = core->getGlobalLogger();
        H_LOG( glog, Logger::WARNING ) << "Atmospheric CO2 will be constrained to user-supplied values!" << std::endl;
    }

    // One-time checks
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        H_ASSERT( beta.at( *it ) >= 0.0, "beta < 0" );
        H_ASSERT( q10_rh.at( *it )>0.0, "q10_rh <= 0.0" );
    }
    sanitychecks();
}

//------------------------------------------------------------------------------
/*! \brief                  Run code, called from core
 *  \param[in] runToDate    Date to which to run to, double
 *
 *  This run method doesn't do much, because it's the carbon-cycle-solver
 *  run that does all the work.
 */
void SimpleNbox::run( const double runToDate ) throw ( h_exception )
{
    in_spinup = core->inSpinup();
    sanitychecks();

    Tgav_record.set( runToDate, core->sendMessage( M_GETDATA, D_GLOBAL_TEMP ).value( U_DEGC ) );
}

//------------------------------------------------------------------------------
/*! \brief                  Spinup run code, called from core
 *  \param[in] step         Spinup step number
 *
 *  This run_spinup method doesn't do much, because it's the carbon-cycle-solver
 *  run that does all the work.
 */
bool SimpleNbox::run_spinup( const int step ) throw ( h_exception )
{
    sanitychecks();
    in_spinup = true;
    return true;        // solver will really be the one signalling
}

//------------------------------------------------------------------------------
// documentation is inherited
unitval SimpleNbox::getData(const std::string& varName,
                            const double date) throw ( h_exception )
{
    unitval returnval;

    std::string biome = SNBOX_DEFAULT_BIOME;
    std::string varNameParsed = varName;
    std::string biome_error = "Biome '" + biome + "' missing from biome list. " +
        "Hit this error while trying to retrieve variable: '" + varName + "'.";

    // Does the varName contain our parse character? If so, split it
    std::vector<std::string> splitvec;
    boost::split( splitvec, varName, boost::is_any_of( SNBOX_PARSECHAR ) );
    H_ASSERT( splitvec.size() < 3, "max of one separator allowed in variable names" );

    if( splitvec.size() == 2 ) {    // i.e., in form <biome>.<varname>
        biome = splitvec[ 0 ];
        varNameParsed = splitvec[ 1 ];
        // Have to re-set this here because `biome` has changed
        biome_error = "Biome '" + biome + "' missing from biome list. " +
            "Hit this error while trying to retrieve variable: '" + varName + "'.";
    }

    if( varNameParsed == D_ATMOSPHERIC_C ) {
        if(date == Core::undefinedIndex())
            returnval = atmos_c;
        else
            returnval = atmos_c_ts.get(date); 
    } else if( varNameParsed == D_ATMOSPHERIC_CO2 ) {
        if(date == Core::undefinedIndex())
            returnval = Ca;
        else
            returnval = Ca_ts.get(date);
    } else if( varNameParsed == D_ATMOSPHERIC_C_RESIDUAL ) {
        if(date == Core::undefinedIndex())
            returnval = residual;
        else
            returnval = residual_ts.get(date);
    } else if( varNameParsed == D_PREINDUSTRIAL_CO2 ) {
        H_ASSERT( date == Core::undefinedIndex(), "Date not allowed for preindustrial CO2" );
        returnval = C0;
    } else if(varNameParsed == D_WARMINGFACTOR) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for biome warming factor");
        H_ASSERT(has_biome( biome ), biome_error);
        returnval = unitval(warmingfactor.at(biome), U_UNITLESS);
    } else if(varNameParsed == D_BETA) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for CO2 fertilization (beta)");
        H_ASSERT(has_biome( biome ), biome_error);
        returnval = unitval(beta.at(biome), U_UNITLESS);
    } else if(varNameParsed == D_Q10_RH) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for Q10");
        returnval = unitval(q10_rh.at( biome ), U_UNITLESS);
    } else if( varNameParsed == D_LAND_CFLUX ) {
        H_ASSERT( date == Core::undefinedIndex(), "Date not allowed for atm-land flux" );
        returnval = sum_npp() - sum_rh() - sum_rh_ch4() - lucEmissions.get( ODEstartdate );

    } else if( varNameParsed == D_RF_T_ALBEDO ) {
        H_ASSERT( date != Core::undefinedIndex(), "Date required for albedo forcing" );
        returnval = Ftalbedo.get( date );

        // Partitioning parameters.
    } else if(varNameParsed == D_F_NPPV) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for vegetation NPP fraction");
        returnval = unitval(f_nppv.at( biome ), U_UNITLESS);
    } else if(varNameParsed == D_F_NPPD) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for detritus NPP fraction");
        returnval = unitval(f_nppd.at( biome ), U_UNITLESS);
    } else if(varNameParsed == D_F_LITTERD) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for litter-detritus fraction");
        returnval = unitval(f_litterd.at( biome ), U_UNITLESS);
    } else if(varNameParsed == D_F_LUCV) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for LUC vegetation fraction");
        returnval = unitval(f_lucv, U_UNITLESS);
    } else if(varNameParsed == D_F_LUCD) {
        H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for LUC detritus fraction");
        returnval = unitval(f_lucd, U_UNITLESS);

    } else if( varNameParsed == D_EARTHC ) {
        if(date == Core::undefinedIndex())
            returnval = earth_c;
        else
            returnval = earth_c_ts.get(date);
    } else if( varNameParsed == D_VEGC ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if(date == Core::undefinedIndex())
                returnval = sum_map( veg_c );
            else
                returnval = sum_map(veg_c_tv.get(date));
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = veg_c.at(biome) ;
            else
                returnval = veg_c_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_DETRITUSC ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if(date == Core::undefinedIndex())
                returnval = sum_map( detritus_c );
            else
                returnval = sum_map(detritus_c_tv.get(date));
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = detritus_c.at(biome) ;
            else
                returnval = detritus_c_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_SOILC ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if(date == Core::undefinedIndex())
                returnval = sum_map( soil_c );
            else
                returnval = sum_map(soil_c_tv.get(date));
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = soil_c.at(biome);
            else
                returnval = soil_c_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_PERMAFROSTC ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if(date == Core::undefinedIndex())
                returnval = sum_map( permafrost_c );
            else
                returnval = sum_map(permafrost_c_tv.get(date));
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = permafrost_c.at(biome);
            else
                returnval = permafrost_c_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_F_FROZEN) {
        double tempval;
        if(biome == SNBOX_DEFAULT_BIOME) {
            if(date == Core::undefinedIndex())
                tempval = sum_map( f_frozen );
            else
                tempval = sum_map(f_frozen_tv.get(date));
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                tempval = f_frozen.at(biome);
            else
                tempval = f_frozen_tv.get(date).at(biome);
        }
        returnval = unitval( tempval, U_UNITLESS );
    } else if( varNameParsed == D_NPP_FLUX0 ) {
      H_ASSERT(date == Core::undefinedIndex(), "Date not allowed for npp_flux0" );
      H_ASSERT(has_biome( biome ), biome_error);
      returnval = npp_flux0.at(biome);
    } else if( varNameParsed == D_FFI_EMISSIONS ) {
        H_ASSERT( date != Core::undefinedIndex(), "Date required for ffi emissions" );
        returnval = ffiEmissions.get( date );
    } else if( varNameParsed == D_LUC_EMISSIONS ) {
        H_ASSERT( date != Core::undefinedIndex(), "Date required for luc emissions" );
        returnval = lucEmissions.get( date );
    } else if( varNameParsed == D_NPP ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if (date == Core::undefinedIndex())
                returnval = sum_map( NPP_veg );
            else
                returnval = sum_map( NPP_veg_tv.get(date) );
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = NPP_veg.at(biome);
            else
                returnval = NPP_veg_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_RH ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if (date == Core::undefinedIndex())
                returnval = sum_map( RH_det ) + sum_map( RH_soil );
            else
                returnval = sum_map( RH_det_tv.get(date) ) + sum_map( RH_soil_tv.get(date) );
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = RH_det.at(biome) + RH_soil.at(biome);
            else
                returnval = RH_det_tv.get(date).at(biome) + RH_soil_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_RH_DETRITUS ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if (date == Core::undefinedIndex())
                returnval = sum_map( RH_det );
            else
                returnval = sum_map( RH_det_tv.get(date) );
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = RH_det.at(biome);
            else
                returnval = RH_det_tv.get(date).at(biome);
        }
    } else if( varNameParsed == D_RH_SOIL ) {
        if(biome == SNBOX_DEFAULT_BIOME) {
            if (date == Core::undefinedIndex())
                returnval = sum_map( RH_soil );
            else
                returnval = sum_map( RH_soil_tv.get(date) );
        } else {
            H_ASSERT(has_biome( biome ), biome_error);
            if(date == Core::undefinedIndex())
                returnval = RH_soil.at(biome);
            else
                returnval = RH_soil_tv.get(date).at(biome);
        }
    } else if ( varNameParsed == D_RH_CH4 ) {
        H_ASSERT( date == Core::undefinedIndex() , "date not allowed for CH4 from RH" );
        returnval = sum_rh_ch4();

    }else {
        H_THROW( "Caller is requesting unknown variable: " + varName );
    }

    return returnval;
}

void SimpleNbox::reset(double time) throw(h_exception)
{
    // Reset all state variables to their values at the reset time
    earth_c = earth_c_ts.get(time);
    atmos_c = atmos_c_ts.get(time);
    Ca = Ca_ts.get(time);

    veg_c = veg_c_tv.get(time);
    detritus_c = detritus_c_tv.get(time);
    soil_c = soil_c_tv.get(time);
    permafrost_c = permafrost_c_tv.get(time);

    residual = residual_ts.get(time);

    tempferts = tempferts_tv.get(time);
    tempfertd = tempfertd_tv.get(time);
    f_frozen = f_frozen_tv.get(time);

    // Calculate derived quantities
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        if(in_spinup) {
            co2fert[ biome ] = 1.0; // co2fert fixed if in spinup.  Placeholder in case we decide to allow resetting into spinup
        }
        else {
            co2fert[ biome ] = calc_co2fert(biome);
        }
    }
    Tgav_record.truncate(time);
    // No need to reset masstot; it's not supposed to change anyhow.

    // Truncate all of the state variable time series
    earth_c_ts.truncate(time);
    atmos_c_ts.truncate(time);
    Ca_ts.truncate(time);

    veg_c_tv.truncate(time);
    detritus_c_tv.truncate(time);
    soil_c_tv.truncate(time);
    permafrost_c_tv.truncate(time);

    residual_ts.truncate(time);

    tempferts_tv.truncate(time);
    tempfertd_tv.truncate(time);

    tcurrent = time;

    H_LOG(logger, Logger::NOTICE)
        << getComponentName() << " reset to time= " << time << "\n";
}

//------------------------------------------------------------------------------
// documentation is inherited
void SimpleNbox::shutDown()
{
    H_LOG( logger, Logger::DEBUG ) << "goodbye " << getComponentName() << std::endl;
    logger.close();
}

//------------------------------------------------------------------------------
/*! \brief visitor accept code
 */
void SimpleNbox::accept( AVisitor* visitor ) {
    visitor->visit( this );
}

//------------------------------------------------------------------------------
/*! \brief            transfer model pools to flat array (for ODE solver)
 *  \param[in] t  time, double, the date from which ODE solver is starting
 *  \param[in] c  flat array of carbon pools (no units)
 */
void SimpleNbox::getCValues( double t, double c[] )
{
    c[ SNBOX_ATMOS ] = atmos_c.value( U_PGC );
    c[ SNBOX_VEG ] = sum_map( veg_c ).value( U_PGC );
    c[ SNBOX_DET ] = sum_map( detritus_c ).value( U_PGC );
    c[ SNBOX_SOIL ] = sum_map( soil_c ).value( U_PGC );
    omodel->getCValues( t, c );
    c[ SNBOX_EARTH ] = earth_c.value( U_PGC );
    c[ SNBOX_PERMAFROST ] = sum_map( permafrost_c ).value( U_PGC );

    ODEstartdate = t;
}

//------------------------------------------------------------------------------
/*! \brief                  Transfer new model pools from ODE solver array back to model pools
 *  \param[in] t            Time, double, the ending date of the solver
 *  \param[in] double       Flat array of carbon pools (no units)
 *  \exception h_exception  If ocean model diverges from our pool tracking all ocean C
 *  \exception h_exception  If mass is not conserved
 *
 *  \details Transfer solver pools (no units) back to our pools (with units), and run
 *  a sanity check to make sure mass has been conserved.
 */
void SimpleNbox::stashCValues( double t, const double c[] )
{
    // Solver has gone from ODEstartdate to t
    const double yf = ( t - ODEstartdate );
    H_ASSERT( yf >= 0 && yf <= 1, "yearfraction out of bounds" );

    H_LOG( logger,Logger::DEBUG ) << "Stashing at t=" << t << ", solver pools at " << t << ": " <<
        "  atm = " << c[ SNBOX_ATMOS ] <<
        "  veg = " << c[ SNBOX_VEG ] <<
        "  det = " << c[ SNBOX_DET ] <<
        "  soil = " << c[ SNBOX_SOIL ] <<
        "  ocean = " << c[ SNBOX_OCEAN ] <<
        "  earth = " << c[ SNBOX_EARTH ] <<
        "  permafrost = " << c[ SNBOX_PERMAFROST ] <<
        std::endl;

    log_pools( t );

    // Store solver pools into our internal variables

    atmos_c.set( c[ SNBOX_ATMOS ], U_PGC );

    // The solver just knows about one vegetation box, one detritus, and one
    // soil. So we need to apportion new veg C pool (set by the solver) to
    // as many biomes as we have. This is not ideal.
    // TODO: Solver actually solves all boxes in multi-biome system

    // Apportioning is done by NPP and RH
    // i.e., biomes with higher values get more of any C change
    const unitval npp_rh_total = sum_npp() + sum_rh(); // these are both positive
    // Use separate weighting and apportioning for permafrost
    const unitval permafrost_total = sum_map( permafrost_c );
    const unitval newveg( c[ SNBOX_VEG ], U_PGC );
    const unitval newdet( c[ SNBOX_DET ], U_PGC );
    const unitval newsoil( c[ SNBOX_SOIL ], U_PGC );
    const unitval newpermafrost( c[ SNBOX_PERMAFROST ], U_PGC );
    unitval veg_delta = newveg - sum_map( veg_c );  // TODO: make const
    unitval det_delta = newdet - sum_map( detritus_c );  // TODO: make const
    unitval soil_delta = newsoil - sum_map( soil_c );  // TODO: make const
    unitval permafrost_delta = newpermafrost - permafrost_total;  // TODO: make const
    H_LOG( logger,Logger::DEBUG ) << "veg_delta = " << veg_delta << std::endl;
    H_LOG( logger,Logger::DEBUG ) << "det_delta = " << det_delta << std::endl;
    H_LOG( logger,Logger::DEBUG ) << "soil_delta = " << soil_delta << std::endl;
    H_LOG( logger,Logger::DEBUG ) << "permafrost_delta = " << permafrost_delta << std::endl;

    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        const double wt     = ( npp( biome ) + rh( biome ) ) / npp_rh_total;
        H_LOG( logger,Logger::DEBUG ) << "Biome " << biome << " weight = " << wt << std::endl;
        // If no permafrost, the weight evaluates to `nan`, so set to zero.
        const double wt_pf  = permafrost_total > 0 ? permafrost_c.at( biome ) / permafrost_total : 0;
        H_LOG( logger,Logger::DEBUG ) << "Biome " << biome << " permafrost weight = " << wt_pf << std::endl;
        veg_c[ biome ]      = veg_c.at( biome ) + veg_delta * wt;
        detritus_c[ biome ] = detritus_c.at( biome ) + det_delta * wt;
        soil_c[ biome ]     = soil_c.at( biome ) + soil_delta * wt;
        permafrost_c[ biome ]     = permafrost_c.at( biome ) + permafrost_delta * wt_pf;
    }

    log_pools( t );

    omodel->stashCValues( t, c );   // tell ocean model to store new C values
    earth_c.set( c[ SNBOX_EARTH ], U_PGC );

    log_pools( t );

    // Each time the model pools are updated, check that mass has been conserved
    double sum=0.0;
    for( int i=0; i<ncpool(); i++ ) {
        sum += c[ i ];
    }

    const double diff = fabs( sum - masstot );
    H_LOG( logger,Logger::DEBUG ) << "masstot = " << masstot << ", sum = " << sum << ", diff = " << diff << std::endl;
    if(masstot > 0.0 && diff > MB_EPSILON) {
        H_LOG( logger,Logger::SEVERE ) << "Mass not conserved in " << getComponentName() << std::endl;
        H_LOG( logger,Logger::SEVERE ) << "masstot = " << masstot << ", sum = " << sum << ", diff = " << diff << std::endl;
        H_THROW( "Mass not conserved! (See log.)" );
    }
    masstot = sum;

    // If user has supplied Ca values, adjust atmospheric C to match
    if(core->inSpinup() ||
       ( Ca_constrain.size() && t <= Ca_constrain.lastdate() )) {

        unitval atmos_cpool_to_match;
        unitval atmppmv;
        if(core->inSpinup()) {
            atmos_cpool_to_match.set(C0.value(U_PPMV_CO2) / PGC_TO_PPMVCO2, U_PGC);
            atmppmv.set(C0.value(U_PPMV_CO2), U_PPMV_CO2);
        }
        else {
            H_LOG( logger, Logger::WARNING ) << "** Constraining atmospheric CO2 to user-supplied value" << std::endl;
            atmos_cpool_to_match.set(Ca_constrain.get(t).value( U_PPMV_CO2 ) /
                                     PGC_TO_PPMVCO2, U_PGC);
            atmppmv.set(Ca_constrain.get(t).value(U_PPMV_CO2), U_PPMV_CO2);
        }

        residual = atmos_c - atmos_cpool_to_match;
        H_LOG( logger,Logger::DEBUG ) << t << "- have " << Ca << " want " <<  atmppmv.value( U_PPMV_CO2 ) << std::endl;
        H_LOG( logger,Logger::DEBUG ) << t << "- have " << atmos_c << " want " << atmos_cpool_to_match << "; residual = " << residual << std::endl;

        // Transfer C from atmosphere to deep ocean and update our C and Ca variables
        H_LOG( logger,Logger::DEBUG ) << "Sending residual of " << residual << " to deep ocean" << std::endl;
        core->sendMessage( M_DUMP_TO_DEEP_OCEAN, D_OCEAN_C, message_data( residual ) );
        atmos_c = atmos_c - residual;
        Ca.set( atmos_c.value( U_PGC ) * PGC_TO_PPMVCO2, U_PPMV_CO2 );
    } else {
        residual.set( 0.0, U_PGC );
    }

    // All good! t will be the start of the next timestep, so
    ODEstartdate = t;
}

// A series of small functions to calculate variables that will appear in the output stream

double SimpleNbox::calc_co2fert(std::string biome, double time) const
{
    unitval Ca_t = time == Core::undefinedIndex() ? Ca : Ca_ts.get(time);
    return 1 + beta.at(biome) * log(Ca_t/C0);
}

//------------------------------------------------------------------------------
/*! \brief      Compute annual net primary production
 *  \returns    current annual NPP
 */
unitval SimpleNbox::npp(std::string biome, double time) const
{
    unitval npp = npp_flux0.at( biome );    // 'at' throws exception if not found
    if(time == Core::undefinedIndex()) {
        npp = npp * co2fert.at( biome );        // that's why used here instead of []
    }
    else {
        npp = npp * calc_co2fert(biome, time);
    }
    return npp;
}

//------------------------------------------------------------------------------
/*! \brief      Compute global net primary production
 *  \returns    Annual NPP summed across all biomes
 */
unitval SimpleNbox::sum_npp(double time) const
{
    unitval total( 0.0, U_PGC_YR );
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        total = total + npp( *it, time );}
    return total;
}

//------------------------------------------------------------------------------
/*! \brief      Compute detritus component of annual heterotrophic respiration
 *  \returns    current detritus component of annual heterotrophic respiration
 */
unitval SimpleNbox::rh_fda( std::string biome ) const
{
    unitval dflux( detritus_c.at( biome ).value( U_PGC ) * 0.25, U_PGC_YR );
    return dflux * tempfertd.at( biome ) * (1.0 - rh_ch4_frac.at( biome ));
}

//------------------------------------------------------------------------------
/*! \brief      Compute detritus component of annual heterotrophic respiration - CH4
 *  \returns    current detritus component of annual heterotrophic respiration - CH4
 */
unitval SimpleNbox::rh_fda_ch4( std::string biome ) const
{
    unitval dflux( detritus_c.at( biome ).value( U_PGC ) * 0.25, U_PGC_YR );
    return dflux * tempfertd.at( biome ) * (rh_ch4_frac.at( biome ));
}

//------------------------------------------------------------------------------
/*! \brief      Compute soil component of annual heterotrophic respiration
 *  \returns    current soil component of annual heterotrophic respiration
 */
unitval SimpleNbox::rh_fsa( std::string biome ) const
{
    unitval soilflux( soil_c.at( biome ).value( U_PGC ) * 0.02, U_PGC_YR );
    return soilflux * tempferts.at( biome ) * (1.0 - rh_ch4_frac.at( biome ));
}

//------------------------------------------------------------------------------
/*! \brief      Compute soil component of annual heterotrophic respiration
 *  \returns    current soil component of annual heterotrophic respiration
 */
unitval SimpleNbox::rh_fsa_ch4( std::string biome ) const
{
    unitval soilflux( soil_c.at( biome ).value( U_PGC ) * 0.02, U_PGC_YR );
    return soilflux * tempferts.at( biome ) * rh_ch4_frac.at( biome );
}

//------------------------------------------------------------------------------
/*! \brief      Compute total annual heterotrophic respiration
 *  \returns    current annual heterotrophic respiration
 */
unitval SimpleNbox::rh( std::string biome ) const
{
    // Heterotrophic respiration is the sum of fluxes from detritus and soil
    return rh_fda( biome ) + rh_fsa( biome );
}

//------------------------------------------------------------------------------
/*! \brief      Compute heterotrophic respiration methane component
 *  \returns    Current annual heterotrophic respiration, as CH4
 */
unitval SimpleNbox::rh_ch4( std::string biome ) const
{
    return rh_fda_ch4( biome ) + rh_fsa_ch4( biome );
}


//------------------------------------------------------------------------------
/*! \brief      Compute global heterotrophic respiration
 *  \returns    Annual RH summed across all biomes
 */
unitval SimpleNbox::sum_rh() const
{
    unitval total( 0.0, U_PGC_YR );
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        total = total + rh( *it );
    }
    return total;
}



//------------------------------------------------------------------------------
/*! \brief      Compute global heterotrophic CH4 respiration
 *  \returns    Annual CH4 RH summed across all biomes
 */
unitval SimpleNbox::sum_rh_ch4() const
{
    unitval total( 0.0, U_PGC_YR );
    for ( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        total = total + rh_ch4( *it );
    }
    return total;
}

//------------------------------------------------------------------------------
/*! \brief              Compute model fluxes for a time step
 *  \param[in]  t       time
 *  \param[in]  c       carbon pools (no units)
 *  \param[out] dcdt    carbon fluxes
 *  \returns            code indicating success or failure
 */
int SimpleNbox::calcderivs( double t, const double c[], double dcdt[] ) const
{
    // Solver is attempting to go from ODEstartdate to t
//    const double yearfraction = ( t - ODEstartdate );
//    H_ASSERT( yearfraction >= 0 && yearfraction <= 1, "yearfraction out of bounds" );

    // Atmosphere-ocean flux is calculated by ocean_component
    const int omodel_err = omodel->calcderivs( t, c, dcdt );
    unitval atmosocean_flux( dcdt[ SNBOX_OCEAN ], U_PGC_YR );

    /// NPP: Net primary productivity
    unitval npp_biome( 0.0, U_PGC_YR);
    unitval npp_current( 0.0, U_PGC_YR );
    unitval npp_fav( 0.0, U_PGC_YR );
    unitval npp_fad( 0.0, U_PGC_YR );
    unitval npp_fas( 0.0, U_PGC_YR );

    // RH: heterotrophic respiration
    unitval rh_fda_current( 0.0, U_PGC_YR );
    unitval rh_fsa_current( 0.0, U_PGC_YR );

    unitval rh_fda_ch4_current( 0.0, U_PGC_YR );
    unitval rh_fsa_ch4_current( 0.0, U_PGC_YR );

    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        // NPP is scaled by CO2 from preindustrial value
        npp_biome = npp( biome );
        npp_current = npp_current + npp_biome;
        npp_fav = npp_fav + npp_biome * f_nppv.at( biome );
        npp_fad = npp_fad + npp_biome * f_nppd.at( biome );
        npp_fas = npp_fas + npp_biome * (1 - f_nppv.at( biome ) - f_nppd.at( biome ));

        rh_fda_current = rh_fda_current + rh_fda( biome );
        rh_fsa_current = rh_fsa_current + rh_fsa( biome );

        rh_fda_ch4_current = rh_fda_ch4_current + rh_fda_ch4( biome );
        rh_fsa_ch4_current = rh_fsa_ch4_current + rh_fsa_ch4( biome );
    }
    unitval rh_current = rh_fda_current + rh_fsa_current;
    unitval rh_ch4_current = rh_fsa_ch4_current + rh_fda_ch4_current;

    // Detritus flux comes from the vegetation pool
    // TODO: these values should use the c[] pools passed in by solver!
    unitval litter_flux( 0.0, U_PGC_YR );
    unitval litter_fvd( 0.0, U_PGC_YR );
    unitval litter_fvs( 0.0, U_PGC_YR );
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        unitval v = unitval( veg_c.at( biome ).value( U_PGC ) * 0.035, U_PGC_YR );
        litter_flux = litter_flux + v;
        litter_fvd = litter_fvd + v * f_litterd.at( biome );
        litter_fvs = litter_fvs + v * ( 1 - f_litterd.at( biome ) );
    }

    // Some detritus goes to soil
    unitval detsoil_flux( 0.0, U_PGC_YR );
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        detsoil_flux = detsoil_flux + unitval( detritus_c.at( biome ).value( U_PGC ) * 0.6, U_PGC_YR );
    }

    // Annual fossil fuels and industry emissions
    unitval ffi_flux_current( 0.0, U_PGC_YR );
    if( !in_spinup ) {   // no perturbation allowed if in spinup
        ffi_flux_current = ffiEmissions.get( t );
    }

    // Annual land use change emissions
    unitval luc_current( 0.0, U_PGC_YR );
    if( !in_spinup ) {   // no perturbation allowed if in spinup
        luc_current = lucEmissions.get( t );
    }

    // Land-use change contribution can come from veg, detritus, and soil
    unitval luc_fva = luc_current * f_lucv;
    unitval luc_fda = luc_current * f_lucd;
    unitval luc_fsa = luc_current * ( 1 - f_lucv - f_lucd );

    // Oxidized methane of fossil fuel origin
    unitval ch4ox_current( 0.0, U_PGC_YR );     //TODO: implement this

    // As permafrost thaws, the C is mobilized into the soil pool.
    unitval permafrost_thaw_c( 0.0, U_PGC_YR );
    if ( !in_spinup ) { // No permafrost thaw during spinup
        // Static (non-labile) C fraction of permafrost
        // TODO: Needs to be a settable param.
        double fpf_static = 0.4;
        // Sum permafrost thaw in all biomes
        for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
            std::string biome = *it;
            double biome_c_thaw = permafrost_c.at(biome).value( U_PGC ) *
                new_thaw.at(biome) * (1 - fpf_static);
            permafrost_thaw_c = permafrost_thaw_c + unitval( biome_c_thaw, U_PGC_YR );
        }
    }

    // Compute fluxes
    dcdt[ SNBOX_ATMOS ] = // change in atmosphere pool
        ffi_flux_current.value( U_PGC_YR )
        + luc_current.value( U_PGC_YR )
        + ch4ox_current.value( U_PGC_YR )
        - atmosocean_flux.value( U_PGC_YR )
        - npp_current.value( U_PGC_YR )
        // HACK: For mass balance purposes, dump both RH{CO2} and RH{CH4} into
        // the atmosphere. Effectivel, this means that CH4 is emitted on top of
        // existing CO2 -- i.e. more CH4 emissions does not mean less CO2
        // emissions from RH). The correct solution is to have a separate, 8th
        // naturally-emitted CH4 box.
        + rh_ch4_current.value( U_PGC_YR )
        + rh_current.value( U_PGC_YR );
    dcdt[ SNBOX_VEG ] = // change in vegetation pool
        npp_fav.value( U_PGC_YR )
        - litter_flux.value( U_PGC_YR )
        - luc_fva.value( U_PGC_YR );
    dcdt[ SNBOX_DET ] = // change in detritus pool
        npp_fad.value( U_PGC_YR )
        + litter_fvd.value( U_PGC_YR )
        - detsoil_flux.value( U_PGC_YR )
        - rh_fda_current.value( U_PGC_YR )
        - rh_fda_ch4_current.value( U_PGC_YR )
        - luc_fda.value( U_PGC_YR );
    dcdt[ SNBOX_SOIL ] = // change in soil pool
        npp_fas.value( U_PGC_YR )
        + litter_fvs.value( U_PGC_YR )
        + detsoil_flux.value( U_PGC_YR )
        + permafrost_thaw_c.value( U_PGC_YR )
        - rh_fsa_current.value( U_PGC_YR )
        - rh_fsa_ch4_current.value( U_PGC_YR )
        - luc_fsa.value( U_PGC_YR );
    dcdt[ SNBOX_OCEAN ] = // change in ocean pool
        atmosocean_flux.value( U_PGC_YR );
    dcdt[ SNBOX_EARTH ] = // change in earth pool
        - ffi_flux_current.value( U_PGC_YR );
    dcdt[ SNBOX_PERMAFROST ] = // change in permafrost pool
     - permafrost_thaw_c.value( U_PGC_YR );

/*    printf( "%6.3f%8.3f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f\n", t, dcdt[ SNBOX_ATMOS ],
            dcdt[ SNBOX_VEG ], dcdt[ SNBOX_DET ], dcdt[ SNBOX_SOIL ], dcdt[ SNBOX_OCEAN ], dcdt[ SNBOX_EARTH ], dcdt[ SNBOX_PERMAFROST ] );
*/
    return omodel_err;
}

//------------------------------------------------------------------------------
/*! \brief              Compute 'slowly varying' fluxes
 *  \param[in]  t       time (at the *beginning* of the current time step.
 *  \param[in]  c       carbon pools (no units)
 *
 *  Compute 'slowly varying' fertilization and anthropogenic fluxes.
 *  Treat the fertilization factors as slowly varying for illustrative purposes
 *  (in fact we could calculate it at each integration step if we wanted to).
 */
void SimpleNbox::slowparameval( double t, const double c[] )
{
    omodel->slowparameval( t, c );      // pass msg on to ocean model

	// CO2 fertilization
    Ca.set( c[ SNBOX_ATMOS ] * PGC_TO_PPMVCO2, U_PPMV_CO2 );

    // Compute CO2 fertilization factor globally (and for each biome specified)
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        if( in_spinup ) {
            co2fert[ biome ] = 1.0;  // no perturbation allowed if in spinup
        } else {
            co2fert[ biome ] = calc_co2fert( biome );
        }
        H_LOG( logger,Logger::DEBUG ) << "co2fert[ " << biome << " ] at " << Ca << " = " << co2fert.at( biome ) << std::endl;
    }

    // Compute temperature factor globally (and for each biome specified)
    // Heterotrophic respiration depends on the pool sizes (detritus and soil) and Q10 values
    // The soil pool uses a lagged Tgav, i.e. we assume it takes time for heat to diffuse into soil
    const double Tgav = core->sendMessage( M_GETDATA, D_GLOBAL_TEMP ).value( U_DEGC );


    /* set tempferts (soil) and tempfertd (detritus) for each biome */

    // Need the previous time step values of tempferts.  Since t is
    // the time at the beginning of the current time step (== the end
    // of the previous time step), we can use t as the index to look
    // up the previous value.
    double_stringmap tfs_last;  // Previous time step values of tempferts; initialized empty
    if(t != Core::undefinedIndex() && t > core->getStartDate()) {
        tfs_last = tempferts_tv[t];
    }

    // Loop over biomes.
    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        if( in_spinup ) {
            tempfertd[ biome ] = 1.0;  // no perturbation allowed in spinup
            tempferts[ biome ] = 1.0;  // no perturbation allowed in spinup
            f_frozen[ biome ] = 1.0;
            new_thaw[ biome ] = 0.0;
        } else {
            double wf;
            if( warmingfactor.count( biome ) ) {
                wf = warmingfactor.at( biome );   // biome-specific warming
            } else if ( warmingfactor.count( SNBOX_DEFAULT_BIOME ) ) {
                wf = warmingfactor.at( SNBOX_DEFAULT_BIOME );
            } else {
                wf = 1.0;
            }

            const double Tgav_biome = Tgav * wf;    // biome-specific temperature

            tempfertd[ biome ] = pow( q10_rh.at( biome ), ( Tgav_biome / 10.0 ) ); // detritus warms with air

            // Permafrost thaw
            // Currently, these are calibrated to produce a 0.172 / year slope from
            // 0.8 to 4 degrees C, which was the linear form of this in Kessler.
            // TODO: These should be settable parameters
            double pf_mu = 1.258;
            double pf_sigma = 0.618;

            new_thaw[ biome ] = 0.0;
            if (permafrost_c[ biome ] > unitval(0.0, U_PGC)) {
#ifdef USE_RCPP
                double f_frozen_current = R::plnorm(Tgav_biome, pf_mu, pf_sigma, 0, 0);
#else
                H_THROW("Permafrost C calculation requires R math library ",
                        "for the `plnorm` function. Please compile with `-DUSE_RCPP` ",
                        "and be sure to link to Rcpp headers and libraries.")
#endif
                new_thaw[ biome ] = f_frozen[ biome ] - f_frozen_current;
                f_frozen[ biome ] = f_frozen_current;
            }

            // Soil warm very slowly relative to the atmosphere
            // We use a mean temperature of a window (size Q10_TEMPN) of temperatures to scale Q10
            #define Q10_TEMPLAG 0 //125         // TODO: put lag in input files 150, 25
            #define Q10_TEMPN 200 //25
            double Tgav_rm = 0.0;       /* window mean of Tgav */
            if( t > core->getStartDate() + Q10_TEMPLAG ) {
                for( int i=t-Q10_TEMPLAG-Q10_TEMPN; i<t-Q10_TEMPLAG; i++ ) {
                    Tgav_rm += Tgav_record.get( i ) * wf;
                }
                Tgav_rm /= Q10_TEMPN;
            }

            tempferts[ biome ] = pow( q10_rh.at( biome ), ( Tgav_rm / 10.0 ) );

            // The soil Q10 effect is 'sticky' and can only increase, not decline
            double tempferts_last = tfs_last[ biome ]; // If tfs_last is empty, this will produce 0.0
            if(tempferts[ biome ] < tempferts_last) {
                tempferts[ biome ] = tempferts_last;
            }

            H_LOG( logger,Logger::DEBUG ) << biome << " Tgav=" << Tgav << ", Tgav_biome=" << Tgav_biome << ", tempfertd=" << tempfertd[ biome ]
                << ", tempferts=" << tempferts[ biome ] << std::endl;
        }
    } // loop over biomes
    // save the new values for use in the next time step
    // TODO:  move this to a purpose-built recording subroutine
    //tempferts_tv.set(tcurrent, tempferts);
    H_LOG(logger, Logger::DEBUG) << "slowparameval: would have recorded tempferts = " << tempferts[SNBOX_DEFAULT_BIOME]
                                 << " at time= " << tcurrent << std::endl;
}

void SimpleNbox::record_state(double t)
{
    tcurrent = t;
    earth_c_ts.set(t, earth_c);
    atmos_c_ts.set(t, atmos_c);
    Ca_ts.set(t, Ca);

    veg_c_tv.set(t, veg_c);
    detritus_c_tv.set(t, detritus_c);
    soil_c_tv.set(t, soil_c);
    permafrost_c_tv.set(t, permafrost_c);

    for( auto it = biome_list.begin(); it != biome_list.end(); it++ ) {
        std::string biome = *it;
        if (!in_spinup) {
            NPP_veg[ biome ] = npp( biome );
            RH_det[ biome ] = rh_fda( biome ) + rh_fda_ch4( biome );
            RH_soil[ biome ] = rh_fsa( biome ) + rh_fsa_ch4( biome );
        } else {
            NPP_veg[ biome ] = unitval(0.0, U_PGC_YR);
            RH_det[ biome ] = unitval(0.0, U_PGC_YR);
            RH_soil[ biome ] = unitval(0.0, U_PGC_YR);
        }
    }
    NPP_veg_tv.set(t, NPP_veg);
    RH_det_tv.set(t, RH_det);
    RH_soil_tv.set(t, RH_soil);

    residual_ts.set(t, residual);

    tempfertd_tv.set(t, tempfertd);
    tempferts_tv.set(t, tempferts);
    f_frozen_tv.set(t, f_frozen);
    H_LOG(logger, Logger::DEBUG) << "record_state: recorded tempferts = " << tempferts[SNBOX_DEFAULT_BIOME]
                                 << " at time= " << t << std::endl;

    // ocean model appears to be controlled by the N-box model.  Seems
    // like it makes swapping out for another model a nightmare, but
    // that's where we're at.
    omodel->record_state(t);

}

// Set the preindustrial carbon value and adjust total mass to reflect the new
// value (unless it hasn't yet been set).  Note that after doing this,
// attempting to run without first doing a reset will cause an exception due to
// failure to conserve mass.
void SimpleNbox::set_c0(double newc0)
{
    if(masstot > 0.0) {
        double massdiff = (newc0 - C0) * PPMVCO2_TO_PGC;
        masstot += massdiff;
        H_LOG(logger, Logger::DEBUG) << "massdiff= " << massdiff << "  new masstot= " << masstot
                                     << "\n";
    }
    C0.set(newc0, U_PPMV_CO2);

}

// Check if `biome` is present in biome_list
bool SimpleNbox::has_biome(const std::string& biome) {
    return std::find(biome_list.begin(), biome_list.end(), biome) != biome_list.end();
}

// Create a new biome, and initialize it with zero C pools and fluxes
// and the same parameters as the most recently created biome.
void SimpleNbox::createBiome(const std::string& biome)
{

    H_LOG(logger, Logger::DEBUG) << "Creating new biome '" << biome << "'." << std::endl;

    // Throw an error if the biome already exists
    std::string errmsg = "Biome '" + biome + "' is already in `biome_list`.";
    H_ASSERT(!has_biome( biome ), errmsg);

    // Initialize new pools
    veg_c[ biome ] = unitval(0, U_PGC);
    add_biome_to_ts(veg_c_tv, biome, veg_c.at( biome ));
    detritus_c[ biome ] = unitval(0, U_PGC);
    add_biome_to_ts(detritus_c_tv, biome, detritus_c.at( biome ));
    soil_c[ biome ] = unitval(0, U_PGC);
    add_biome_to_ts(soil_c_tv, biome, soil_c.at( biome ));
    permafrost_c[ biome ] = unitval(0, U_PGC);
    add_biome_to_ts(permafrost_c_tv, biome, permafrost_c.at( biome ));

    NPP_veg[ biome ] = unitval(0.0, U_PGC);
    add_biome_to_ts(NPP_veg_tv, biome, NPP_veg.at( biome ));
    RH_det[ biome ] = unitval(0.0, U_PGC);
    add_biome_to_ts(RH_det_tv, biome, RH_det.at( biome ));
    RH_soil[ biome ] = unitval(0.0, U_PGC);
    add_biome_to_ts(RH_soil_tv, biome, RH_soil.at( biome ));

    npp_flux0[ biome ] = unitval(0, U_PGC_YR);

    // Other defaults (these will be re-calculated later)
    co2fert[ biome ] = 1.0;
    tempfertd[ biome ] = 1.0;
    add_biome_to_ts(tempfertd_tv, biome, 1.0);
    tempferts[ biome ] = 1.0;
    add_biome_to_ts(tempferts_tv, biome, 1.0);

    std::string last_biome = biome_list.back();

    // Set parameters to same as most recent biome
    beta[ biome ] = beta[ last_biome ];
    q10_rh[ biome ] = q10_rh[ last_biome ];
    warmingfactor[ biome ] = warmingfactor[ last_biome ];
    f_nppv[ biome ] = f_nppv[ last_biome ];
    f_nppd[ biome ] = f_nppd[ last_biome ];
    f_litterd[ biome ] = f_litterd[ last_biome ];
    rh_ch4_frac[ biome ] = rh_ch4_frac[ last_biome ];

    // Add to end of biome list
    biome_list.push_back(biome);

    H_LOG(logger, Logger::DEBUG) << "Finished creating biome '" << biome << "'." << std::endl;}

// Delete a biome: Remove it from the `biome_list` and `erase` all of
// the associated parameters.
void SimpleNbox::deleteBiome(const std::string& biome) // Throw an error if the biome already exists
{

    H_LOG(logger, Logger::DEBUG) << "Deleting biome '" << biome << "'." << std::endl;

    std::string errmsg = "Biome '" + biome + "' not found in `biome_list`.";
    std::vector<std::string>::const_iterator i_biome = std::find(biome_list.begin(), biome_list.end(), biome);
    H_ASSERT(has_biome( biome ), errmsg);

    // Erase all values associated with the biome:
    // Parameters
    beta.erase( biome );
    q10_rh.erase(biome);
    warmingfactor.erase( biome );
    f_nppv.erase(biome);
    f_nppd.erase(biome);
    f_litterd.erase(biome);
    rh_ch4_frac.erase(biome);

    // C pools
    veg_c.erase( biome );
    remove_biome_from_ts(veg_c_tv, biome);
    detritus_c.erase( biome );
    remove_biome_from_ts(detritus_c_tv, biome);
    soil_c.erase( biome );
    remove_biome_from_ts(soil_c_tv, biome);
    permafrost_c.erase( biome );
    remove_biome_from_ts(permafrost_c_tv, biome);

    // C fluxes
    NPP_veg.erase( biome );
    remove_biome_from_ts(NPP_veg_tv, biome);
    RH_det.erase( biome );
    remove_biome_from_ts(RH_det_tv, biome);
    RH_soil.erase( biome );
    remove_biome_from_ts(RH_soil_tv, biome);

    // Others
    npp_flux0.erase( biome );
    tempfertd.erase( biome );
    remove_biome_from_ts( tempfertd_tv, biome );
    tempferts.erase( biome );
    remove_biome_from_ts( tempferts_tv, biome );
    co2fert.erase( biome );

    // Remove from `biome_list`
    biome_list.erase( i_biome );

    H_LOG(logger, Logger::DEBUG) << "Finished deleting biome '" << biome << ",." << std::endl;

}

// Create a new biome called `newname`, transfer all of the parameters
// and pools from `oldname`, and delete `oldname`. Note that the new
// biome will be at the end of the `biome_list`.
void SimpleNbox::renameBiome(const std::string& oldname, const std::string& newname)
{
    H_LOG(logger, Logger::DEBUG) << "Renaming biome '" << oldname <<
        "' to '" << newname << "'." << std::endl;

    std::string errmsg = "Biome '" + oldname + "' not found in `biome_list`.";
    H_ASSERT(has_biome( oldname ), errmsg);
    errmsg = "Biome '" + newname + "' already exists in `biome_list`.";
    H_ASSERT(!has_biome( newname ), errmsg);

    beta[ newname ] = beta.at( oldname );
    beta.erase(oldname);
    q10_rh[ newname ] = q10_rh.at( oldname );
    q10_rh.erase(oldname);
    warmingfactor[ newname ] = warmingfactor.at( oldname );
    warmingfactor.erase( oldname );
    f_nppv[ newname ] = f_nppv.at( oldname );
    f_nppv.erase(oldname);
    f_nppd[ newname ] = f_nppd.at( oldname );
    f_nppd.erase(oldname);
    f_litterd[ newname ] = f_litterd.at( oldname );
    f_litterd.erase(oldname);
    rh_ch4_frac[ newname ] = rh_ch4_frac.at( oldname );
    rh_ch4_frac.erase(oldname);

    H_LOG(logger, Logger::DEBUG) << "Transferring C from biome '" << oldname <<
        "' to '" << newname << "'." << std::endl;

    // Transfer all C from `oldname` to `newname`
    veg_c[ newname ] = veg_c.at( oldname );
    veg_c.erase(oldname);
    rename_biome_in_ts(veg_c_tv, oldname, newname);
    detritus_c[ newname ] = detritus_c.at( oldname );
    detritus_c.erase(oldname);
    rename_biome_in_ts(detritus_c_tv, oldname, newname);
    soil_c[ newname ] = soil_c.at( oldname );
    soil_c.erase(oldname);
    rename_biome_in_ts(soil_c_tv, oldname, newname);
    permafrost_c[ newname ] = permafrost_c.at( oldname );
    permafrost_c.erase(oldname);
    rename_biome_in_ts(permafrost_c_tv, oldname, newname);

    NPP_veg[ newname ] = NPP_veg.at( oldname );
    NPP_veg.erase(oldname);
    rename_biome_in_ts(NPP_veg_tv, oldname, newname);
    RH_det[ newname ] = RH_det.at( oldname );
    RH_det.erase(oldname);
    rename_biome_in_ts(RH_det_tv, oldname, newname);
    RH_soil[ newname ] = RH_soil.at( oldname );
    RH_soil.erase(oldname);
    rename_biome_in_ts(RH_soil_tv, oldname, newname);

    npp_flux0[ newname ] = npp_flux0.at( oldname );
    npp_flux0.erase(oldname);

    co2fert[ newname ] = co2fert[ oldname ];
    co2fert.erase( oldname );

    tempfertd[ newname ] = tempfertd[ oldname ];
    tempfertd.erase(oldname);
    tempferts[ newname ] = tempferts[ oldname ];
    tempferts.erase(oldname);

    biome_list.push_back(newname);
    biome_list.erase(std::find(biome_list.begin(), biome_list.end(), oldname));

    H_LOG(logger, Logger::DEBUG) << "Done renaming biome '" << oldname <<
        "' to '" << newname << "'." << std::endl;

}

}

