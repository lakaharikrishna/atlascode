#ifndef EXPLODE_H
#define EXPLODE_H

#include <string>
#include <vector>

/**
 * @brief Splits a string into a vector of substrings using the given delimiter.
 *
 * @param str The input string to be split.
 * @param delim The character used as a separator.
 * @return std::vector<std::string> Vector containing substrings.
 *
 * Example:
 *      auto parts = explode("part1:part2:part3", ':');
 *      // parts = {"part1", "part2", "part3"}
 */

enum RequestStatus
{
    REQUESTED = 0,              // New request
    REQUEST_QUEUED = 1,         // Queued for processing
    IN_PROGRESS = 2,            // Processing started
    SUCCESS_STATUS = 3,         // Successfully completed
    RETRY_IN_PROGRESS = 4,      // Retry attempt
    CANCELLED = 5,              // Cancelled by user
    FAILED_INVALID_REQUEST = 6, // Invalid request
    FAILED_RF_TIMEOUT = 7,      // RF timeout
    FAILED_NO_GW_RESPONSE = 8,  // No gateway response
    FAILED_PARTIAL_DATA = 9,    // Partial data received
    GW_DISCONNECTED = 10,       // GW Disconnected
    DLMS_FAILED = 11,           // DLMS connection failed
    FAILED_PMESH_ERROR = 12,    // PMESH protocol error
    DLMS_ERROR = 13,            // ALL DLMS ERRORS
};

std::vector<std::string> explode(const std::string &str, char delim);

#endif // EXPLODE_H
