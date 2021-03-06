context('Test changes to Hector parameters')

inputdir <- system.file('input', package='hector')
sampledir <- system.file('output', package='hector')
testvars <- c(ATMOSPHERIC_CO2(), RF_TOTAL(), GLOBAL_TEMP())
dates <- 1750:2100
rcp45 <- file.path(inputdir, "hector_rcp45.ini")

test_that('Rerunning spinup produces minimal change', {
    hc <- newcore(file.path(inputdir, 'hector_rcp45.ini'), suppresslogging = TRUE)
    run(hc, 2100)
    dd1 <- fetchvars(hc, dates, testvars)

    ## reset to before start date, but do not change anything
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, dates, testvars)

    diff <- abs((dd2$value - dd1$value) / (dd1$value + 1.0e-6))
    expect_lt(max(diff), 1.0e-6)

    shutdown(hc)
})

test_that('Initial CO2 concentration equals preindustrial', {
    hc <- newcore(rcp45, suppresslogging=TRUE)
    run(hc, 1800)
    initcval <- fetchvars(hc, 1745, ATMOSPHERIC_CO2())
    preind <- fetchvars(hc, NA, PREINDUSTRIAL_CO2())
    expect_equal(initcval$value, preind$value)

    setvar(hc, NA, PREINDUSTRIAL_CO2(), 285, "ppmv CO2")
    reset(hc)
    run(hc, 1800)
    initcval <- fetchvars(hc, 1745, ATMOSPHERIC_CO2())
    preind <- fetchvars(hc, NA, PREINDUSTRIAL_CO2())
    expect_equal(initcval$value, preind$value)
})


test_that('Lowering initial CO2 lowers output CO2', {
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    dd1 <- fetchvars(hc, dates, ATMOSPHERIC_CO2())

    ## Change the preindustrial CO2
    setvar(hc, NA, PREINDUSTRIAL_CO2(), 250, "ppmv CO2")
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, dates, ATMOSPHERIC_CO2())

    ## The concentrations should start off lower than the reference run by
    ## approximately the change in initial concentration, and the deficit
    ## increases with time.  We'll test for this approximately
    diff <- dd2$value - dd1$value       # should all be negative
    expect_lt(max(diff), -26.0)

    shutdown(hc)
})


test_that('Lowering ECS lowers output Temperature', {
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    ## temperature bounces around a bit in the early years; limit test to after
    ## 2000, when the signal is clear
    tdates <- 2000:2100
    dd1 <- fetchvars(hc, tdates, GLOBAL_TEMP())

    setvar(hc, NA, ECS(), 2.5, 'degC')
    ## make sure this still works with automatic reset.
    run(hc, 2100)
    dd2 <- fetchvars(hc, tdates, GLOBAL_TEMP())

    ## Check that temperature is lower
    diff <- dd2$value - dd1$value
    expect_lt(max(diff), 0.0)

    shutdown(hc)
})

test_that('Raising Q10 increases CO2 concentration', {
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100                 # limit to years where temperature
                                        # increase is smooth.
    dd1 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    ## Change the preindustrial CO2
    setvar(hc, NA, Q10_RH(), 2.5, NA)
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    ## Check that concentration is higher across the board
    diff <- dd2$value - dd1$value
    expect_gt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Lowering diffusivity increases temperature', {
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100                 # limit to years where temperature
                                        # increase is smooth.
    dd1 <- fetchvars(hc, qdates, GLOBAL_TEMP())

    ## Change the diffusivity
    setvar(hc, NA, DIFFUSIVITY(), 2.0, "cm2/s")
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, GLOBAL_TEMP())

    ## Check that concentration is higher across the board
    diff <- dd2$value - dd1$value
    expect_gt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Lowering aerosol forcing scaling factor increases temperature', {
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100

    dd1 <- fetchvars(hc, qdates, GLOBAL_TEMP())

    ## Change the aerosol scaling factor
    setvar(hc, NA, AERO_SCALE(), 0.5, NA)
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, GLOBAL_TEMP())

    ## Check that temperatures are higher
    diff <- dd2$value - dd1$value
    expect_gt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Increasing volcanic forcing scaling factor increases the effect of volcanism', {
    ## Use the difference between 1960 temperature and 1965 temperature as a
    ## measure of the volcanic effect.
    hc <- newcore(rcp45, suppresslogging=TRUE)
    run(hc, 1971)
    dates <- c(1960, 1965)
    tbase <- fetchvars(hc, dates, GLOBAL_TEMP())
    vsibase <- tbase$value[1] - tbase$value[2]

    setvar(hc, NA, VOLCANIC_SCALE(), 1.5, getunits(VOLCANIC_SCALE()))
    reset(hc)
    run(hc, 1971)
    tscl <- fetchvars(hc, dates, GLOBAL_TEMP())
    vsiscl <- tscl$value[1] - tscl$value[2]

    expect_gt(vsiscl, vsibase)
})

test_that('Decreasing vegetation NPP fraction increases CO2 concentration', {
    # More NPP to vegetation means less C to soil, where it decomposes.
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100                 # limit to years where temperature
                                        # increase is smooth.
    dd1 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    setvar(hc, NA, F_NPPV(), 0.25, NA) # Default: 0.35
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    ## Check that concentration is higher across the board
    diff <- dd2$value - dd1$value
    expect_gt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Decreasing detritus NPP fraction decreases CO2 concentration', {
    # Less detritus C means more soil C, which decomposes slower
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100                 # limit to years where temperature
                                        # increase is smooth.
    dd1 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    setvar(hc, NA, F_NPPD(), 0.5, NA) # Default: 0.60
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    ## Check that concentration is lower across the board
    diff <- dd2$value - dd1$value
    expect_lt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Decreasing litter flux to detritus decreases CO2 concentrations ', {
    # Less detritus C means more soil C, which decomposes slower
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100                 # limit to years where temperature
                                        # increase is smooth.
    dd1 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    setvar(hc, NA, F_LITTERD(), 0.9, NA) # Default = 0.98
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, ATMOSPHERIC_CO2())

    ## Check that concentration is lower across the board
    diff <- dd2$value - dd1$value
    expect_lt(min(diff), 0.0)

    shutdown(hc)
})

test_that('Increasing CO2 fertilization factor increases NPP', {
    # More sensitive to atmospheric CO2 more NPP
    hc <- newcore(rcp45, suppresslogging = TRUE)
    run(hc, 2100)
    qdates <- 2000:2100
    # increase is smooth.
    dd1 <- fetchvars(hc, qdates, NPP())

    setvar(hc, NA, BETA(), 0.75, NA) # Default = 0.36
    reset(hc, 0.0)
    run(hc, 2100)
    dd2 <- fetchvars(hc, qdates, NPP())

    ## Check that NPP is larger across the board
    diff <- dd2$value - dd1$value
    expect_gt(min(diff), 0.0)

    shutdown(hc)
})

test_that('All "fraction" parameters can be set and retrieved', {
    # Less detritus C means more soil C, which decomposes slower
    hc <- newcore(rcp45, suppresslogging = TRUE)
    fracs <- list(
      F_NPPV = 0.4, # default 0.35
      F_NPPD = 0.5, # default 0.60
      F_LITTERD = 0.9, # default 0.98
      F_LUCV = 0.2, # default 0.1
      F_LUCD = 0.02 # default = 0.01
    )
    # Set names to result of calling corresponding function (e.g. `F_NPPV()`)
    names(fracs) <- lapply(names(fracs), do.call, args = list())
    for (i in seq_along(fracs)) {
      setvar(hc, NA, names(fracs)[[i]], fracs[[i]], NA)
    }
    out <- fetchvars(hc, NA, names(fracs))
    expect_equivalent(as.list(out$value), fracs)
    shutdown(hc)
})

test_that('Atmospheric CO2 concentrations can be constrained', {

    hc <- newcore(rcp45, suppresslogging = TRUE)
    years <- 1850:2100
    constrained_values <- rep(300, length(years))
    setvar(hc, years, CO2_CONSTRAIN(), constrained_values, getunits(CO2_CONSTRAIN()))
    invisible(run(hc))
    out <- fetchvars(hc, years, ATMOSPHERIC_CO2())
    expect_equal(out$value, constrained_values)

})

test_that('Atmospheric CO2 constraint affects RF and temperature', {

    # Checks for the regression reoported here:
    # https://github.com/JGCRI/hector/pull/302#issuecomment-493242815
    hc <- newcore(rcp45, suppresslogging = TRUE)
    ca_years <- 1850:2100
    ca_low <- rep(278, length(ca_years))
    setvar(hc, ca_years, CO2_CONSTRAIN(), ca_low, getunits(CO2_CONSTRAIN()))
    invisible(run(hc))
    out_ca_lo <- fetchvars(hc, ca_years, ATMOSPHERIC_CO2())
    expect_equal(out_ca_lo$value, ca_low)
    out_rf_lo <- fetchvars(hc, ca_years, RF_CO2())
    out_tgav_lo <- fetchvars(hc, ca_years, GLOBAL_TEMP())

    ca_hi <- ca_low * 3
    reset(hc)
    setvar(hc, ca_years, CO2_CONSTRAIN(), ca_hi, getunits(CO2_CONSTRAIN()))
    invisible(run(hc))
    out_ca_hi <- fetchvars(hc, ca_years, ATMOSPHERIC_CO2())
    expect_equal(out_ca_hi$value, ca_hi)
    out_rf_hi <- fetchvars(hc, ca_years, RF_CO2())
    out_tgav_hi <- fetchvars(hc, ca_years, GLOBAL_TEMP())

    expect_true(all(out_rf_hi$value > out_rf_lo$value))
    expect_true(all(out_tgav_hi$value > out_tgav_lo$value))

})

test_that('Discontinuous constraint works', {

    hc <- newcore(rcp45, suppresslogging = TRUE)
    all_years <- seq(startdate(hc), enddate(hc))
    ca_years_1 <- 1850:1860
    ca_vals_1 <- rep(278, length(ca_years_1))
    setvar(hc, ca_years_1, CO2_CONSTRAIN(), ca_vals_1, getunits(CO2_CONSTRAIN()))
    ca_years_2 <- 1870:1880
    ca_vals_2 <- rep(298, length(ca_years_2))
    setvar(hc, ca_years_2, CO2_CONSTRAIN(), ca_vals_2, getunits(CO2_CONSTRAIN()))
    invisible(run(hc))

    # Make sure we can retrieve all these values
    out_rf <- fetchvars(hc, all_years, RF_CO2())
    out_ca <- fetchvars(hc, all_years, ATMOSPHERIC_CO2())

    # RFs and concentrations during the constraint period are constant
    expect_equal(length(unique(out_rf[out_rf$year %in% ca_years_1, "value"])), 1)
    expect_equal(length(unique(out_rf[out_rf$year %in% ca_years_2, "value"])), 1)
    expect_equal(out_ca[out_ca$year %in% ca_years_1, "value"], ca_vals_1)
    expect_equal(out_ca[out_ca$year %in% ca_years_2, "value"], ca_vals_2)

    # Expect CO2 concentration and RF to increase -- i.e. they are NOT constant
    expect_true(out_rf[out_ca$year == 2000, "value"] >
                    out_rf[out_ca$year == 1900, "value"])
    expect_true(out_ca[out_ca$year == 2000, "value"] >
                    out_ca[out_ca$year == 1900, "value"])

    # You should be able to retrieve constraints that were set
    expect_equal(fetchvars(hc, min(ca_years_1), CO2_CONSTRAIN())$value, ca_vals_1[1])
    expect_equal(fetchvars(hc, max(ca_years_1), CO2_CONSTRAIN())$value, ca_vals_1[1])
    expect_equal(fetchvars(hc, min(ca_years_2), CO2_CONSTRAIN())$value, ca_vals_2[1])
    expect_equal(fetchvars(hc, max(ca_years_2), CO2_CONSTRAIN())$value, ca_vals_2[1])

    # You should NOT be able to retrieve constraints before, between, or after
    # user-specified values.
    baddates <- c(min(ca_years_1) - 1, max(ca_years_1) + 1, max(ca_years_2) + 1)
    ca_before <- fetchvars(hc, baddates, CO2_CONSTRAIN())
    expect_true(all(is.na(ca_before$value)))
    expect_true(all(!is.nan(ca_before$value)))

})
