// Test-only native MODEL fixture for the Python mesh CLI integration test.
// This helper is not a production worker operation and is not installed.
#include <QCoreApplication>
#include <QFileInfo>
#include <rml_model.h>
#include <rbl_error.h>

int main(int argc,char **argv)
{
    QCoreApplication application(argc,argv);
    const auto arguments=application.arguments();
    if (arguments.size()!=2 || !QFileInfo(arguments[1]).isAbsolute()
        || QFileInfo::exists(arguments[1]) || QFileInfo(arguments[1]).suffix()!=QStringLiteral("rbm")) return 2;
    try
    {
        RModel model; model.setNNodes(4); model.setNSurfaces(1);
        model.getSurface(0).setName("SDK analytic tetrahedron boundary");
        model.getSurface(0).setMaterial(RMaterial::generateDefault());
        const double xyz[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
        for (uint i=0;i<4;++i)
        { model.getNode(i).set(xyz[i][0],xyz[i][1],xyz[i][2]); model.setNodePhysicalId(i,101+i); }
        const uint faces[4][3]={{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
        for (uint i=0;i<4;++i)
        {
            RElement element(R_ELEMENT_TRI1);
            for (uint j=0;j<3;++j) element.setNodeId(j,faces[i][j]);
            model.addElement(element,true,0); model.setElementPhysicalId(i,501+i);
        }
        model.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
        model.write(arguments[1],false);
        return QFileInfo(arguments[1]).isFile() ? 0 : 3;
    }
    catch (const RError &) { return 3; }
    catch (...) { return 3; }
}
