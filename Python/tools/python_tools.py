"""
Python Tools for Unreal MCP.

Runs Python inside the Unreal Editor through the PythonScriptPlugin.
"""

import logging
import os
import tempfile
from typing import Dict, Any
from mcp.server.fastmcp import FastMCP, Context

logger = logging.getLogger("UnrealMCP")

def register_python_tools(mcp: FastMCP):
    """Register Python tools with the MCP server."""

    @mcp.tool()
    def execute_python(ctx: Context, code: str = "", file: str = "") -> Dict[str, Any]:
        """
        Run Python in the Unreal Editor (the `unreal` module is available) and return its printed output.

        Args:
            code: Python source to run. Written to a temp file first, so any length works.
            file: Path to an existing .py file to run instead of code.

        Returns:
            Response with 'output' (everything printed or logged) and 'result'.
        """
        from unreal_mcp_server import get_unreal_connection

        path = file
        if not path:
            if not code:
                return {"success": False, "message": "Pass code or file"}
            fd, path = tempfile.mkstemp(suffix=".py", prefix="mcp_")
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(code)
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            response = unreal.send_command("execute_python", {"file": path.replace("\\", "/")})
            return response or {"success": False, "message": "No response from Unreal Engine"}
        except Exception as e:
            logger.error(f"Error executing python: {e}")
            return {"success": False, "message": str(e)}
        finally:
            if not file:
                try:
                    os.remove(path)
                except OSError:
                    pass

    logger.info("Python tools registered successfully")
