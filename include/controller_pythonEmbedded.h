/*
 * Copyright (C) 2016 Emmanuel Durand
 *
 * This file is part of Splash.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Splash is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Splash.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * @pythonEmbedded.h
 * The PythonEmbedded class, which runs a Python 3.x controller
 */

#ifndef SPLASH_PYTHON_EMBEDDED_H
#define SPLASH_PYTHON_EMBEDDED_H

#include <future>
#include <string>
#include <thread>

#include "./coretypes.h"
#include "./basetypes.h"
#include "./controller.h"

namespace Splash {

/*************/
class PythonEmbedded : public ControllerObject
{
    public:
        /**
         * \brief Constructor
         */
        PythonEmbedded(std::weak_ptr<RootObject> root);

        /**
         * \brief Destructor
         */
        ~PythonEmbedded();

        /**
         * \brief Set the path to the source Python file
         * \param filepath Path to the source file
         * \return Return true if the file exists
         */
        bool setScriptFile(const std::string& filepath);

        /**
         * \brief Run the script
         * \return Return true if the script launched successfully
         */
        bool run();

        /**
         * \brief Stop the running script
         * \return Return true if the script ran and exited correctly
         */
        bool stop();

    private:
        std::string _filepath {""}; //!< Path to the python script
        std::string _scriptName {""}; //!< Name of the module (filename minus .py)

        bool _doLoop {false}; //!< Set to false to stop the Python loop
        std::thread _loopThread {}; //!< Python thread loop
        std::promise<bool> _loopThreadPromise {}; //!< Holds the output result from the threading loop

        /**
         * \brief Python interpreter main loop
         */
        void loop();

        /**
         * \brief Register new functors to modify attributes
         */
        void registerAttributes();
};

} // end of namespace

#endif // SPLASH_PYTHON_EMBEDDED_H
