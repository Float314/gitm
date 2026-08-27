#pragma once

#include <string>
#include <vector>

// SMTP server settings used when sending gitm notifications.
struct smtp_config {
    std::string host;
    int port = 587;
    bool use_tls = false; // true for implicit TLS (SMTPS, typically port 465)
    std::string username;
    std::string password;
};

// An outgoing email message.
struct mail_message {
    std::string from;
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::vector<std::string> bcc;
    std::string subject;
    std::string body;
};

// Sends `message` through the SMTP server described by `config`. Returns true
// on success; on failure false is returned and `error` is filled.
bool send_email(const smtp_config& config, const mail_message& message, std::string& error);

// Resolves a decent SMTP server + account for `email` when we only know the
// user's address (used by login-through-mail). Returns true when a guess is
// available.
bool guess_smtp_config(const std::string& email, const std::string& password, smtp_config& out);
