/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <fc/crypto/sha256.hpp>
#include <boost/algorithm/string.hpp>
#include <evt/chain/exceptions.hpp>
#include <evt/wallet_plugin/wallet_manager.hpp>
#include <evt/wallet_plugin/wallet.hpp>
#include <evt/wallet_plugin/se_wallet.hpp>

namespace evt {
namespace wallet {

constexpr auto file_ext        = ".wallet";
constexpr auto password_prefix = "PW";

std::string
gen_password() {
    auto key = private_key_type::generate();
    return password_prefix + string(key);
}

bool
valid_filename(const string& name) {
    if (name.empty()) return false;
    if (std::find_if(name.begin(), name.end(), !boost::algorithm::is_alnum() && !boost::algorithm::is_any_of("._-")) != name.end()) return false;
    return boost::filesystem::path(name).filename().string() == name;
}

wallet_manager::wallet_manager() {
#ifdef __APPLE__
   try {
      wallets.emplace("SecureEnclave", std::make_unique<se_wallet>());
   }
   catch(fc::exception& ) {}
#endif
}

wallet_manager::~wallet_manager() {
    //not really required, but may spook users
    if(wallet_dir_lock) {
        boost::filesystem::remove(lock_path);
    }
}

void
wallet_manager::set_timeout(const std::chrono::seconds& t) {
    timeout = t;
    auto now = std::chrono::system_clock::now();
    timeout_time = now + timeout;
    EVT_ASSERT(timeout_time >= now && timeout_time.time_since_epoch().count() > 0, invalid_lock_timeout_exception, "Overflow on timeout_time, specified ${t}, now ${now}, timeout_time ${timeout_time}",
        ("t", t.count())("now", now.time_since_epoch().count())("timeout_time", timeout_time.time_since_epoch().count()));
}

void
wallet_manager::check_timeout() {
    if(timeout_time != timepoint_t::max()) {
        const auto& now = std::chrono::system_clock::now();
        if(now >= timeout_time) {
            lock_all();
        }
        timeout_time = now + timeout;
    }
}

std::string
wallet_manager::create(const std::string& name) {
    check_timeout();
    
    EVT_ASSERT(valid_filename(name), wallet_exception, "Invalid filename, path not allowed in wallet name ${n}", ("n", name));

    auto wallet_filename = dir / (name + file_ext);

    if(fc::exists(wallet_filename)) {
        EVT_THROW(chain::wallet_exist_exception, "Wallet with name: '${n}' already exists at ${path}", ("n", name)("path", fc::path(wallet_filename)));
    }

    auto password = gen_password();
    
    auto d      = wallet_data();
    auto wallet = make_unique<soft_wallet>(d);
    wallet->set_password(password);
    wallet->set_wallet_filename(wallet_filename.string());
    wallet->unlock(password);
    wallet->lock();
    wallet->unlock(password);

    // Explicitly save the wallet file here, to ensure it now exists.
    wallet->save_wallet_file();

    // If we have name in our map then remove it since we want the emplace below to replace.
    // This can happen if the wallet file is removed while evtd is running.
    auto it = wallets.find(name);
    if(it != wallets.end()) {
        wallets.erase(it);
    }
    wallets.emplace(name, std::move(wallet));

    return password;
}

void
wallet_manager::open(const std::string& name) {
    check_timeout();

    EVT_ASSERT(valid_filename(name), wallet_exception, "Invalid filename, path not allowed in wallet name ${n}", ("n", name));

    wallet_data d;
    auto        wallet          = std::make_unique<soft_wallet>(d);
    auto        wallet_filename = dir / (name + file_ext);
    wallet->set_wallet_filename(wallet_filename.string());
    if(!wallet->load_wallet_file()) {
        EVT_THROW(chain::wallet_nonexistent_exception, "Unable to open file: ${f}", ("f", wallet_filename.string()));
    }

    // If we have name in our map then remove it since we want the emplace below to replace.
    // This can happen if the wallet file is added while evtd is running.
    auto it = wallets.find(name);
    if(it != wallets.end()) {
        wallets.erase(it);
    }
    wallets.emplace(name, std::move(wallet));
}

std::vector<std::string>
wallet_manager::list_wallets() {
    check_timeout();
    std::vector<std::string> result;
    for(const auto& i : wallets) {
        if(i.second->is_locked()) {
            result.emplace_back(i.first);
        }
        else {
            result.emplace_back(i.first + " *");
        }
    }
    return result;
}

map<public_key_type, private_key_type>
wallet_manager::list_keys(const string& name, const string& pw) {
    check_timeout();

    if(wallets.count(name) == 0)
        EVT_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", name));
    auto& w = wallets.at(name);
    if(w->is_locked())
        EVT_THROW(chain::wallet_locked_exception, "Wallet is locked: ${w}", ("w", name));
    w->check_password(pw);  //throws if bad password
    return w->list_keys();
}

flat_set<public_key_type>
wallet_manager::get_public_keys() {
    check_timeout();
    EVT_ASSERT(!wallets.empty(), wallet_not_available_exception, "You don't have any wallet!");
    flat_set<public_key_type> result;
    bool                      is_all_wallet_locked = true;
    for(const auto& i : wallets) {
        if(!i.second->is_locked()) {
            result.merge(i.second->list_public_keys());
        }
        is_all_wallet_locked &= i.second->is_locked();
    }
    EVT_ASSERT(!is_all_wallet_locked, wallet_locked_exception, "You don't have any unlocked wallet!");
    return result;
}

flat_set<signature_type>
wallet_manager::get_my_signatures(const chain_id_type& chain_id) {
    check_timeout();
    EVT_ASSERT(!wallets.empty(), wallet_not_available_exception, "You don't have any wallet!");
    auto result = flat_set<signature_type>();

    bool is_all_wallet_locked = true;
    for(const auto& i : wallets) {
        if(!i.second->is_locked()) {
            const auto& keys = i.second->list_keys();
            for(const auto& i : keys) {
                result.emplace(i.second.sign(chain_id));
            }
        }
        is_all_wallet_locked &= i.second->is_locked();
    }
    EVT_ASSERT(!is_all_wallet_locked, wallet_locked_exception, "You don't have any unlocked wallet!");
    return result;
}

void
wallet_manager::lock_all() {
    // no call to check_timeout since we are locking all anyway
    for(auto& i : wallets) {
        if(!i.second->is_locked()) {
            i.second->lock();
        }
    }
}

void
wallet_manager::lock(const std::string& name) {
    check_timeout();
    if(wallets.count(name) == 0) {
        EVT_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", name));
    }
    auto& w = wallets.at(name);
    if(w->is_locked()) {
        return;
    }
    w->lock();
}

void
wallet_manager::unlock(const std::string& name, const std::string& password) {
    check_timeout();
    if(wallets.count(name) == 0) {
        open(name);
    }
    auto& w = wallets.at(name);
    if(!w->is_locked()) {
        EVT_THROW(chain::wallet_unlocked_exception, "Wallet is already unlocked: ${w}", ("w", name));
        return;
    }
    w->unlock(password);
}

void
wallet_manager::import_key(const std::string& name, const std::string& wif_key) {
    check_timeout();
    if(wallets.count(name) == 0) {
        EVT_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", name));
    }
    auto& w = wallets.at(name);
    if(w->is_locked()) {
        EVT_THROW(chain::wallet_locked_exception, "Wallet is locked: ${w}", ("w", name));
    }
    w->import_key(wif_key);
}

void
wallet_manager::remove_key(const std::string& name, const std::string& password, const std::string& key) {
    check_timeout();
    if(wallets.count(name) == 0) {
        EVT_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", name));
    }
    auto& w = wallets.at(name);
    if(w->is_locked()) {
        EVT_THROW(chain::wallet_locked_exception, "Wallet is locked: ${w}", ("w", name));
    }
    w->check_password(password); //throws if bad password
    w->remove_key(key);
}

string
wallet_manager::create_key(const std::string& name, const std::string& key_type) {
    check_timeout();
    if(wallets.count(name) == 0) {
        EVT_THROW(chain::wallet_nonexistent_exception, "Wallet not found: ${w}", ("w", name));
    }
    auto& w = wallets.at(name);
    if(w->is_locked()) {
        EVT_THROW(chain::wallet_locked_exception, "Wallet is locked: ${w}", ("w", name));
    }

    string upper_key_type = boost::to_upper_copy<std::string>(key_type);
    return w->create_key(upper_key_type);
}

chain::signed_transaction
wallet_manager::sign_transaction(const chain::signed_transaction& txn, const flat_set<public_key_type>& keys, const chain::chain_id_type& id) {
    check_timeout();
    chain::signed_transaction stxn(txn);

    for(const auto& pk : keys) {
        bool found = false;
        for(const auto& i : wallets) {
            if(!i.second->is_locked()) {
                auto sig = i.second->try_sign_digest(stxn.sig_digest(id), pk);
                if(sig.has_value()) {
                    stxn.signatures.push_back(*sig);
                    found = true;
                    break;  // inner for
                }
            }
        }
        if(!found) {
            EVT_THROW(chain::wallet_missing_pub_key_exception, "Public key not found in unlocked wallets ${k}", ("k", pk));
        }
    }

    return stxn;
}

chain::signature_type
wallet_manager::sign_digest(const chain::digest_type& digest, const public_key_type& key) {
    check_timeout();

    try {
        for(const auto& i : wallets) {
            if(!i.second->is_locked()) {
                auto sig = i.second->try_sign_digest(digest, key);
                if(sig.has_value()) {
                    return *sig;
                }
            }
        }
    }
    FC_LOG_AND_RETHROW();

    EVT_THROW(chain::wallet_missing_pub_key_exception, "Public key not found in unlocked wallets ${k}", ("k", key));
}

void
wallet_manager::own_and_use_wallet(const string& name, std::unique_ptr<wallet_api>&& wallet) {
    if(wallets.find(name) != wallets.end()) {
        FC_THROW("tried to use wallet name the already existed");
    }
    wallets.emplace(name, std::move(wallet));
}

void
wallet_manager::initialize_lock() {
    //This is technically somewhat racy in here -- if multiple evtd are in this function at once.
    //I've considered that an acceptable tradeoff to maintain cross-platform boost constructs here
    lock_path = dir / "wallet.lock";
    {
        std::ofstream x(lock_path.string());
        EVT_ASSERT(!x.fail(), wallet_exception, "Failed to open wallet lock file at ${f}", ("f", lock_path.string()));
    }
    wallet_dir_lock = std::make_unique<boost::interprocess::file_lock>(lock_path.string().c_str());
    if(!wallet_dir_lock->try_lock()) {
        wallet_dir_lock.reset();
        EVT_THROW(wallet_exception, "Failed to lock access to wallet directory; is another evtwd running?");
    }
}

}}  // namespace evt::wallet
