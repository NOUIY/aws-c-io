#include "windows_error_message.h"
#include <Windows.h>
#include <aws/io/logging.h>

void aws_win_log_message(
    enum aws_log_level log_level,
    aws_log_subject_t subject,
    char *function_name,
    DWORD last_error) {
    WCHAR buffer[512]; // Buffer for text.
    DWORD dw_chars;
    dw_chars = FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_error, 0, (LPSTR)buffer, 512, NULL);
    AWS_LOGF(
        log_level,
        subject,
        "Windows system function %s failed with error code %d (%ws)\n",
        function_name,
        last_error,
        dw_chars ? buffer : L"Error message not found.");
}