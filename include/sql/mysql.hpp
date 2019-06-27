#pragma once

#include <climits>
#include <iostream>
#include <list>
#include <map>
#include <string_view>
#include <utility>
#include <vector>
#include "sql/entity.hpp"
#include "sql/type_mapping.hpp"
#include "sql/utility.hpp"

namespace ormpp {

class mysql {
public:
    ~mysql() {
        disconnect();
    }

    template <typename... Args>
    bool connect(Args&&... args) {
        if (con_ != nullptr) {
            mysql_close(con_);
        }

        con_ = mysql_init(nullptr);
        if (con_ == nullptr) {
            set_last_error("mysql init failed");
            return false;
        }

        int timeout = -1;
        auto tp = std::tuple_cat(get_tp(timeout, std::forward<Args>(args)...), std::make_tuple(0, nullptr, 0));

        if (timeout > 0) {
            if (mysql_options(con_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0) {
                set_last_error(mysql_error(con_));
                return false;
            }
        }

        char value = 1;
        mysql_options(con_, MYSQL_OPT_RECONNECT, &value);
        mysql_options(con_, MYSQL_SET_CHARSET_NAME, "utf8");

        if (std::apply(&mysql_real_connect, tp) == nullptr) {
            set_last_error(mysql_error(con_));
            return false;
        }

        return true;
    }

    void set_last_error(std::string last_error) {
        last_error_ = std::move(last_error);
        std::cout << last_error_ << std::endl;  //todo, write to log file
    }

    std::string get_last_error() const {
        return last_error_;
    }

    bool ping() {
        return mysql_ping(con_) == 0;
    }

    template <typename... Args>
    bool disconnect(Args&&... args) {
        if (con_ != nullptr) {
            mysql_close(con_);
            con_ = nullptr;
        }

        return true;
    }

    //for tuple and string with args...
    template <typename T, typename Arg, typename... Args>
    constexpr std::vector<T> query(const Arg& s, Args&&... args) {
        static_assert(iguana::is_tuple<T>::value);
        constexpr auto SIZE = std::tuple_size_v<T>;

        std::string sql = s;
        constexpr auto Args_Size = sizeof...(Args);
        if (Args_Size != 0) {
            if (Args_Size != std::count(sql.begin(), sql.end(), '?')) {
                has_error_ = true;
                return {};
            }

            sql = get_sql(sql, std::forward<Args>(args)...);
        }

        stmt_ = mysql_stmt_init(con_);
        if (!stmt_) {
            has_error_ = true;
            return {};
        }

        if (mysql_stmt_prepare(stmt_, sql.c_str(), (int)sql.size())) {
            fprintf(stderr, "%s\n", mysql_error(con_));
            has_error_ = true;
            return {};
        }

        auto guard = guard_statment(stmt_);

        std::array<MYSQL_BIND, result_size<T>::value> param_binds = {};
        std::list<std::vector<char>> mp;

        std::vector<T> v;
        T tp{};

        size_t index = 0;
        iguana::for_each(tp, [&param_binds, &mp, &index](auto& item, auto /* I */) {
            using U = std::remove_reference_t<decltype(item)>;
            if constexpr (std::is_arithmetic_v<U>) {
                param_binds[index].buffer_type = (enum_field_types)ormpp_mysql::type_to_id(identity<U>{});
                param_binds[index].buffer = &item;
                index++;
            } else if constexpr (std::is_same_v<std::string, U>) {
                std::vector<char> tmp(65536, 0);
                mp.emplace_back(std::move(tmp));
                param_binds[index].buffer_type = MYSQL_TYPE_STRING;
                param_binds[index].buffer = &(mp.back()[0]);
                param_binds[index].buffer_length = 65536;
                index++;
            } else if constexpr (is_char_array_v<U>) {
                param_binds[index].buffer_type = MYSQL_TYPE_VAR_STRING;
                std::vector<char> tmp(sizeof(U), 0);
                mp.emplace_back(std::move(tmp));
                param_binds[index].buffer = &(mp.back()[0]);
                param_binds[index].buffer_length = (unsigned long)sizeof(U);
                index++;
            } else {
                std::cout << typeid(U).name() << std::endl;
            }
        },
                         std::make_index_sequence<SIZE>{});

        if (mysql_stmt_bind_result(stmt_, &param_binds[0])) {
            //                fprintf(stderr, "%s\n", mysql_error(con_));
            has_error_ = true;
            return {};
        }

        if (mysql_stmt_execute(stmt_)) {
            //                fprintf(stderr, "%s\n", mysql_error(con_));
            has_error_ = true;
            return {};
        }

        while (mysql_stmt_fetch(stmt_) == 0) {
            auto it = mp.begin();
            iguana::for_each(tp, [&mp, &it](auto& item, auto /* i */) {
                using U = std::remove_reference_t<decltype(item)>;
                if constexpr (std::is_same_v<std::string, U>) {
                    item = std::string(&(*it)[0], strlen((*it).data()));
                    it++;
                } else if constexpr (is_char_array_v<U>) {
                    memcpy(item, &(*it)[0], sizeof(U));
                }
            },
                             std::make_index_sequence<SIZE>{});

            v.push_back(std::move(tp));
        }

        return v;
    }

    bool has_error() {
        return has_error_;
    }

    //just support execute string sql without placeholders
    bool execute(const std::string& sql) {
        if (mysql_query(con_, sql.data()) != 0) {
            fprintf(stderr, "%s\n", mysql_error(con_));
            return false;
        }

        return true;
    }

    //transaction
    bool begin() {
        if (mysql_query(con_, "BEGIN")) {
            //                fprintf(stderr, "%s\n", mysql_error(con_));
            return false;
        }

        return true;
    }

    bool commit() {
        if (mysql_query(con_, "COMMIT")) {
            //                fprintf(stderr, "%s\n", mysql_error(con_));
            return false;
        }

        return true;
    }

    bool rollback() {
        if (mysql_query(con_, "ROLLBACK")) {
            //                fprintf(stderr, "%s\n", mysql_error(con_));
            return false;
        }

        return true;
    }

private:
    struct guard_statment {
        guard_statment(MYSQL_STMT* stmt) : stmt_(stmt) {}
        MYSQL_STMT* stmt_ = nullptr;
        int status_ = 0;
        ~guard_statment() {
            if (stmt_ != nullptr)
                status_ = mysql_stmt_close(stmt_);

            if (status_)
                fprintf(stderr, "close statment error code %d\n", status_);
        }
    };
	
    template <typename... Args>
    auto get_tp(int& timeout, Args&&... args) {
        auto tp = std::make_tuple(con_, std::forward<Args>(args)...);
        if constexpr (sizeof...(Args) == 5) {
            auto [c, s1, s2, s3, s4, i] = tp;
            timeout = i;
            return std::make_tuple(c, s1, s2, s3, s4);
        } else
            return tp;
    }

    MYSQL* con_ = nullptr;
    MYSQL_STMT* stmt_ = nullptr;
    bool has_error_ = false;
    std::string last_error_;
    inline static std::map<std::string, std::string> auto_key_map_;
};
}  // namespace ormpp