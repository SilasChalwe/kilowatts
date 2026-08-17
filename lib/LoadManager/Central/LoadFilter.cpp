/**
 * @file LoadFilter.cpp
 * @brief Implements the separation of known Loads into Fixed and Auto groups.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#include "LoadFilter.h"

namespace kilowatts {


LoadFilter::LoadFilter()
    : fixedOnLoads_(),
      fixedOffLoads_(),
      autoCandidateLoads_()
{
}


void LoadFilter::reset()
{
    fixedOnLoads_.clear();
    fixedOffLoads_.clear();
    autoCandidateLoads_.clear();
}


bool LoadFilter::addLoad(const Load& load)
{
    if (load.getMode() == LoadMode::Fixed::ON) {
        fixedOnLoads_.push_back(&load);
        return true;
    }

    if (load.getMode() == LoadMode::Fixed::OFF) {
        fixedOffLoads_.push_back(&load);
        return true;
    }

    /*
     * AUTO_ON and AUTO_OFF are both Auto candidate Loads: an Auto Load's
     * current ON/OFF state is only this planning cycle's starting point,
     * not the filter's decision. Best-First Search decides later whether
     * it stays ON or gets switched.
     */
    if (load.isAuto()) {
        autoCandidateLoads_.push_back(&load);
        return true;
    }

    return false;
}


std::size_t LoadFilter::getNumberOfFixedOnLoads() const
{
    return fixedOnLoads_.size();
}


std::size_t LoadFilter::getNumberOfFixedOffLoads() const
{
    return fixedOffLoads_.size();
}


std::size_t LoadFilter::getNumberOfAutoCandidateLoads() const
{
    return autoCandidateLoads_.size();
}


const Load* LoadFilter::getFixedOnLoad(std::size_t loadIndex) const
{
    if (loadIndex >= fixedOnLoads_.size()) {
        return nullptr;
    }

    return fixedOnLoads_[loadIndex];
}


const Load* LoadFilter::getFixedOffLoad(std::size_t loadIndex) const
{
    if (loadIndex >= fixedOffLoads_.size()) {
        return nullptr;
    }

    return fixedOffLoads_[loadIndex];
}


const Load* LoadFilter::getAutoCandidateLoad(std::size_t loadIndex) const
{
    if (loadIndex >= autoCandidateLoads_.size()) {
        return nullptr;
    }

    return autoCandidateLoads_[loadIndex];
}


} // namespace kilowatts
