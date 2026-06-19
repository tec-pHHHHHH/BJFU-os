#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "include/os_types.h"
#include "include/user.h"

/* ================================================================
 * Password hash — lightweight iterative ARX hash (no external deps)
 * Output: 64 hex chars, stored in hash_out[0..63], hash_out[64]='\0'
 * ================================================================ */

void user_hash_password(const char *password, char *hash_out) {
    unsigned int h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    size_t len = strlen(password);
    unsigned int a, b, c, d, e, f, g, i;
    size_t round, byte_idx;

    for (round = 0; round < 8; round++) {
        a = h[0]; b = h[1]; c = h[2]; d = h[3];
        e = h[4]; f = h[5]; g = h[6]; i = h[7];

        for (byte_idx = 0; byte_idx < len; byte_idx++) {
            unsigned char ch = (unsigned char)password[byte_idx];
            a = (a + (ch ^ (unsigned int)(round * 0x9e3779b9 + byte_idx)));
            a = (a << 7)  | (a >> 25);
            b ^= a;
            c = (c + b + 0x9e3779b9);
            c = (c << 11) | (c >> 21);
            d ^= c;
            e = (e + d + (unsigned int)(round + 1));
            e = (e << 13) | (e >> 19);
            f ^= e;
            g = (g + f + (unsigned int)(byte_idx + 1));
            g = (g << 17) | (g >> 15);
            i ^= g;
        }

        h[0] ^= a; h[1] ^= b; h[2] ^= c; h[3] ^= d;
        h[4] ^= e; h[5] ^= f; h[6] ^= g; h[7] ^= i;
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; i = h[7];

    unsigned int len_lo = (unsigned int)(len & 0xFFFFFFFF);
    unsigned int len_hi = (unsigned int)((len >> 32) & 0xFFFFFFFF);

    a += len_lo;  e += len_hi;
    c = (c << 9)  | (c >> 23);
    g = (g << 19) | (g >> 13);
    b ^= a; f ^= e; d += c; i += g;

    h[0] ^= a; h[1] ^= b; h[2] ^= c; h[3] ^= d;
    h[4] ^= e; h[5] ^= f; h[6] ^= g; h[7] ^= i;

    snprintf(hash_out, 65, "%08x%08x%08x%08x%08x%08x%08x%08x",
             h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
}

/* ================================================================
 * Find user by name — traverse linked list
 * ================================================================ */

User* user_find(SystemState *s, const char *username) {
    User *u = s->user_list;
    while (u) {
        if (strcmp(u->username, username) == 0)
            return u;
        u = u->next;
    }
    return NULL;
}

/* ================================================================
 * Register new user
 * ================================================================ */

int user_register(SystemState *s, const char *username, const char *password) {
    if (!username || strlen(username) == 0) return USER_EMPTY_NAME;
    if (!password || strlen(password) == 0) return USER_EMPTY_PWD;

    for (size_t i = 0; username[i]; i++) {
        if (!isalnum((unsigned char)username[i]) && username[i] != '_')
            return USER_EMPTY_NAME;
    }

    if (user_find(s, username)) return USER_EXISTS;

    int count = 0;
    User *u = s->user_list;
    while (u) { count++; u = u->next; }
    if (count >= MAX_USERS) return USER_FULL;

    User *new_user = malloc(sizeof(User));
    if (!new_user) { perror("malloc"); return USER_FULL; }

    strncpy(new_user->username, username, MAX_NAME_LEN - 1);
    new_user->username[MAX_NAME_LEN - 1] = '\0';

    user_hash_password(password, new_user->password_hash);

    new_user->is_locked      = 0;
    new_user->login_attempts = 0;
    new_user->next           = NULL;

    new_user->next = s->user_list;
    s->user_list   = new_user;
    s->dirty       = 1;

    user_save_to_file(s);

    return USER_OK;
}

/* ================================================================
 * User login
 * ================================================================ */

int user_login(SystemState *s, const char *username, const char *password) {
    if (s->current_user[0] != '\0') return USER_OK;

    User *u = user_find(s, username);
    if (!u) return USER_NOT_FOUND;

    if (u->is_locked) return USER_LOCKED;

    char hash[MAX_PASSWORD_LEN];
    user_hash_password(password, hash);

    if (strcmp(u->password_hash, hash) != 0) {
        u->login_attempts++;
        if (u->login_attempts >= 3) {
            u->is_locked = 1;
            printf("[System] Account '%s' is now LOCKED! (3 wrong attempts)\n", username);
        }
        user_save_to_file(s);
        return USER_WRONG_PWD;
    }

    u->login_attempts = 0;
    strncpy(s->current_user, username, MAX_NAME_LEN - 1);
    s->current_user[MAX_NAME_LEN - 1] = '\0';

    printf("[System] Welcome '%s'! Login successful.\n", username);
    return USER_OK;
}

/* ================================================================
 * User logout
 * ================================================================ */

void user_logout(SystemState *s) {
    if (s->current_user[0] == '\0') {
        printf("[System] Not currently logged in.\n");
        return;
    }
    printf("[System] '%s' logged out.\n", s->current_user);
    s->current_user[0] = '\0';
}

/* ================================================================
 * Save user DB to binary file
 *
 * Format: [int count] [User1] [User2] ... [UserN]
 * Each User: username[32] + password_hash[65] + is_locked(int) + attempts(int)
 * ================================================================ */

int user_save_to_file(SystemState *s) {
    FILE *fp = fopen(USER_DB_FILE, "wb");
    if (!fp) {
        perror("user_save_to_file");
        return -1;
    }

    int count = 0;
    User *u = s->user_list;
    while (u) { count++; u = u->next; }

    fwrite(&count, sizeof(int), 1, fp);

    u = s->user_list;
    while (u) {
        fwrite(u->username,       sizeof(char), MAX_NAME_LEN,    fp);
        fwrite(u->password_hash,  sizeof(char), MAX_PASSWORD_LEN, fp);
        fwrite(&u->is_locked,     sizeof(int),  1, fp);
        fwrite(&u->login_attempts, sizeof(int), 1, fp);
        u = u->next;
    }

    fclose(fp);
    return 0;
}

/* ================================================================
 * Load user DB from binary file
 * ================================================================ */

int user_load_from_file(SystemState *s) {
    FILE *fp = fopen(USER_DB_FILE, "rb");
    if (!fp) return 0;

    int count = 0;
    if (fread(&count, sizeof(int), 1, fp) != 1 || count <= 0 || count > MAX_USERS) {
        fclose(fp);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        User *u = malloc(sizeof(User));
        if (!u) { perror("malloc"); fclose(fp); return -1; }

        if (fread(u->username,       sizeof(char), MAX_NAME_LEN,    fp) != (size_t)MAX_NAME_LEN ||
            fread(u->password_hash,  sizeof(char), MAX_PASSWORD_LEN, fp) != (size_t)MAX_PASSWORD_LEN ||
            fread(&u->is_locked,     sizeof(int),  1, fp) != 1 ||
            fread(&u->login_attempts, sizeof(int), 1, fp) != 1) {
            free(u);
            fclose(fp);
            return -1;
        }

        u->username[MAX_NAME_LEN - 1]       = '\0';
        u->password_hash[MAX_PASSWORD_LEN - 1] = '\0';

        u->next       = s->user_list;
        s->user_list  = u;
    }

    fclose(fp);
    printf("[Info] Loaded %d user account(s).\n", count);
    return count;
}
