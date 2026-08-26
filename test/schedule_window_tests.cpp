#include "BestFirstSearch.h"
#include "CurrentTimeProvider.h"
#include "Load.h"
#include "LoadScheduleEvaluator.h"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace kilowatts;

namespace {

Load makeAutoLoad(
    std::uint8_t relayPin,
    float powerWatts,
    std::uint16_t priority)
{
    return Load(
        Load::Id{{0x02U, 0x00U, 0x00U, 0x00U, 0x00U, relayPin}, relayPin},
        "Test Load",
        powerWatts,
        priority,
        LoadPowerType::DC,
        LoadMode::Auto::OFF);
}

void testNormalScheduleWindow()
{
    const AutoSchedule schedule{true, 14U, 0U, 16U, 0U};

    assert(!LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 13U, 59U));
    assert(LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 14U, 0U));
    assert(LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 15U, 59U));
    assert(!LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 16U, 0U));
}

void testOvernightScheduleWindow()
{
    const AutoSchedule schedule{true, 22U, 0U, 2U, 0U};

    assert(!LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 21U, 59U));
    assert(LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 22U, 0U));
    assert(LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 23U, 30U));
    assert(LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 1U, 59U));
    assert(!LoadScheduleEvaluator::isTimeWithinSchedule(schedule, 2U, 0U));
}

void testScheduleValidation()
{
    Load load = makeAutoLoad(1U, 25.0F, 5U);

    assert(load.setSchedule(AutoSchedule{true, 8U, 30U, 10U, 0U}));

    const AutoSchedule stored = load.getSchedule();
    assert(stored.enabled);
    assert(stored.startHour == 8U);
    assert(stored.startMinute == 30U);
    assert(stored.endHour == 10U);
    assert(stored.endMinute == 0U);

    assert(!load.setSchedule(AutoSchedule{true, 8U, 0U, 8U, 0U}));
    assert(!load.setSchedule(AutoSchedule{true, 24U, 0U, 8U, 0U}));
}

void testPriorityControlsSelectedCombination()
{
    CurrentTimeProvider currentTimeProvider;

    Load highPriority = makeAutoLoad(1U, 100.0F, 10U);
    Load lowPriorityA = makeAutoLoad(2U, 50.0F, 1U);
    Load lowPriorityB = makeAutoLoad(3U, 50.0F, 1U);

    const std::vector<const Load*> automaticLoads{
        &highPriority,
        &lowPriorityA,
        &lowPriorityB
    };

    const BestFirstSearch search(
        100.0F,
        automaticLoads,
        currentTimeProvider);

    const std::vector<const Load*>& selected = search.getBestCombination();

    assert(selected.size() == 1U);
    assert(selected.front() == &highPriority);
    assert(highPriority.getPriority() == 10U);
}

} // namespace

int main()
{
    testNormalScheduleWindow();
    testOvernightScheduleWindow();
    testScheduleValidation();
    testPriorityControlsSelectedCombination();
    return 0;
}
