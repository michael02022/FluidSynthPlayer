/**
 * @file
 * The <C>Mutex</C> specification and body provides a mutual exclusion
 * object type.
 *
 * @author Dr. Thomas Tensi
 * @date   2026-10
 */

/*====================*/

#pragma once

/*=========*/
/* IMPORTS */
/*=========*/

#include <mutex>

/*====================*/

namespace BaseTypes::Primitives {

    /**
     * The <C>Mutex</C> type implements mutual exclusion of
     * critical sections.
     */
    struct Mutex : std::mutex {

        /**
         * Locks mutex when <C>lockIsUsed</C> is set, otherwise does
         * nothing.
         *
         * @param[in] lockIsUsed  tells whether lock shall be acquired
         */
        void conditional_lock (IN Boolean lockIsUsed) {
            if (lockIsUsed) {
                lock();
            }
        }

        /*--------------------*/

        /**
         * Tries to lock mutex when <C>lockIsUsed</C> is set and
         * returns whether acquisition was successful; otherwise does
         * nothing and returns success
         *
         * @param[in] lockIsUsed  tells whether lock shall be tried
         *                        for locking
         * @return success of locking
         */
        Boolean conditional_try_lock (IN Boolean lockIsUsed) {
            Boolean result = true;

            if (lockIsUsed) {
                result = try_lock();
            }

            return result;
        }

        /*--------------------*/

        /**
         * Unlocks mutex when <C>lockIsUsed</C> is set, otherwise does
         * nothing.
         *
         * @param[in] lockIsUsed  tells whether lock shall be freed
         */
        void conditional_unlock (IN Boolean lockIsUsed) {
            if (lockIsUsed) {
                unlock();
            }
        }

    };

}
